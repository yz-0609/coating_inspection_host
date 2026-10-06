/**
 * 文件功能：服务进程入口与线程生命周期管理
 *
 * 职责说明：读取配置，组装仪器、工位、业务和输入线程，并处理退出信号。
 * 调用关系：操作系统启动 main → 加载 Config → 创建事件队列 → 启动各模块 → 有序停止。
 * 阅读提示：从 main 向下阅读；self-pipe 是用管道把异步信号转换为 poll 可观察的事件。
 */
#include "inspection/cli.hpp"
#include "inspection/worker.hpp"
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <unistd.h>
// 模块一：退出信号桥接。匿名命名空间使这些变量和函数仅在本文件可见。
namespace {
// 信号处理函数只置 sig_atomic_t 标志和写非阻塞 self-pipe；不使用数据库、日志或 join。
volatile std::sig_atomic_t interrupted = 0;
volatile std::sig_atomic_t signal_write = -1;
// 信号可能打断任意线程；只执行允许的简单操作，将实际清理留给正常控制流。
void signal_handler(int) {
    // 保存并恢复 errno，避免管道写操作污染被信号打断代码的错误状态。
    const int saved_errno = errno;
    interrupted = 1;
    if (signal_write >= 0) {
        char value = 1;
        auto ignored = write(signal_write, &value, 1);
        (void)ignored;
    }
    errno = saved_errno;
}
} // namespace
// 模块二：启动与对象组装。argc 是参数数量，argv 是命令行字符串数组。
int main(int argc, char** argv) {
    using namespace inspection;
    // 要求明确提供配置文件，错误用法返回 2；正常返回 0，系统故障返回 1。
    if (argc != 3 || std::string(argv[1]) != "--config") {
        std::cerr << "用法：inspection_service --config 配置文件\n";
        return 2;
    }
    int stop_pipe[2] = {-1, -1};
    try {
        // 先校验全部配置，成功后才创建通信对象，避免半启动状态。
        Config cfg = load_config(argv[2]);
        // 非阻塞管道避免信号处理函数因写满而等待；CLOEXEC 防止执行其他程序时继承描述符。
        if (pipe2(stop_pipe, O_NONBLOCK | O_CLOEXEC) != 0)
            throw std::runtime_error("self-pipe 创建失败");
        signal_write = stop_pipe[1];
        // 安装 SIGTERM/SIGINT 处理器，分别对应服务停止和终端 Ctrl+C。
        struct sigaction action{};
        action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
        // 容量实验让 SQLite 获得真实写错误，而不是被 SIGXFSZ 直接终止。
        struct sigaction file_signal{};
        file_signal.sa_handler = SIG_IGN;
        if (cfg.allow_test_controls)
            sigaction(SIGXFSZ, &file_signal, nullptr);
        // 模块三：共享基础设施和事件出口。下列对象比各线程先创建、后销毁。
        SystemClock clock;
        BoundedQueue<Event> queue(cfg.queue_capacity);
        BusinessHealth health;
        CandidateDisplay display;
        // 所有生产者共用入口；工位快照在入队时绑定已展示候选，避免稍后的新测量改变按钮含义。
        auto sink = [&](Event e) {
            if (e.kind == EventKind::Snapshot)
                e.bound_candidate = display.current();
            return queue.push(std::move(e));
        };
        // 工厂把配置转为具体后端，主程序只依赖统一接口。
        auto instrument = make_instrument(cfg, clock, sink);
        auto workstation = make_workstation(cfg, clock, health, sink);
        InspectionWorker worker(cfg, clock, queue, health, display, *instrument, *workstation);
        // 先启动消费者，再启动采集/工位生产者；数据库恢复在业务线程中完成。
        worker.start();
        instrument->start();
        workstation->start();
        // CLI 有独立停止标志，stdin 关闭只结束输入线程，不等于结束后台服务。
        std::atomic_bool cli_stopping{false};
        std::thread input([&] {
            cli_loop(STDIN_FILENO, stop_pipe[0], sink, [&] { return cli_stopping.load(); });
        });
        // quit 也由业务线程请求停止；主线程定期观察业务结束事件。
        // 单独停止标志让 CLI EOF 不退出 systemd 服务。
        // 模块四：主线程监督循环。每 100 ms 检查信号或业务结束，poll 避免忙循环。
        while (!interrupted) {
            // service_stopped 无须通过日志反向判断：worker 的完成标志提供生命周期接口。
            if (worker.finished())
                break;
            pollfd fd{stop_pipe[0], POLLIN, 0};
            poll(&fd, 1, 100);
        }
        // 模块五：有序退出。先让业务保存中止并收尾命令，此时设备线程仍能送回命令结果。
        worker.request_stop();
        worker.join();
        // 业务 join 完成后停止设备线程，避免设备回调访问已经销毁的队列。
        health.enabled.store(false);
        instrument->stop();
        workstation->stop();
        // 给输入线程设停止标志并写管道唤醒；join 后再关闭队列及文件描述符。
        cli_stopping.store(true);
        char value = 1;
        auto ignored = write(stop_pipe[1], &value, 1);
        (void)ignored;
        if (input.joinable())
            input.join();
        queue.close();
        close(stop_pipe[0]);
        close(stop_pipe[1]);
        signal_write = -1;
        return worker.failed() ? 1 : 0;
    // 启动或清理失败统一输出 JSON 日志；只关闭已创建的描述符并返回失败码。
    } catch (const std::exception& e) {
        log("startup_error", e.what());
        for (int fd : stop_pipe)
            if (fd >= 0)
                close(fd);
        signal_write = -1;
        return 1;
    }
}
