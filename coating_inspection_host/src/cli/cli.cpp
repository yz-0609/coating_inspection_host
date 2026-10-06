/**
 * 文件功能：命令行文本读取与参数切分
 *
 * 职责说明：用 poll 同时监听标准输入和停止管道，把完整行封装成事件。
 * 调用关系：输入线程执行 cli_loop → 队列 → 业务线程调用 cli_words 解析参数。
 * 阅读提示：一次 read 可能含多行，也可能只有半行，必须用 line 跨次保存。
 */
#include "inspection/cli.hpp"
#include <cerrno>
#include <iomanip>
#include <poll.h>
#include <sstream>
#include <unistd.h>
namespace inspection {
// 模块一：带引号的参数解析。istringstream 将整行当成可逐项读取的字符串输入流。
std::vector<std::string> cli_words(const std::string& line) {
    std::istringstream s(line);
    std::vector<std::string> result;
    std::string word;
    // std::quoted 支持双引号内的空格；普通未加引号的参数仍按空白分隔。
    while (s >> std::quoted(word))
        result.push_back(word);
    // 未到输入末尾却解析失败属于引号格式错误；抛异常让业务入口处理。
    if (!s.eof() && s.fail())
        throw std::runtime_error("命令引号格式无效");
    return result;
}
// 模块二：可唤醒的输入循环。只组装文本/投递事件，不在此处做数据库或设备操作。
void cli_loop(int input_fd, int stop_fd, EventSink sink, const std::function<bool()>& stopping) {
    std::string line;
    while (!stopping()) {
        // 同时监视输入和退出管道；100 ms 超时还让 stopping 标志有机会被检查。
        pollfd fds[2] = {{input_fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
        int ready = poll(fds, 2, 100);
        if (ready < 0) {
            // poll 被信号打断时重新等待；其他系统错误结束输入循环。
            if (errno == EINTR)
                continue;
            break;
        }
        // 停止管道有数据即退出，无需再读取或解释命令。
        if (fds[1].revents & POLLIN)
            break;
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            // 一次最多读 512 字节；系统 read 返回实际字节数，不保证有完整行。
            char data[512];
            auto count = read(input_fd, data, sizeof(data));
            if (count <= 0)
                break;
            for (ssize_t i = 0; i < count; ++i) {
                // 遇到换行才形成 Cli 事件；std::move 转移文本所有权，清空后收集下一行。
                if (data[i] == '\n') {
                    Event e;
                    e.kind = EventKind::Cli;
                    e.text = line;
                    sink(std::move(e));
                    line.clear();
                // 忽略 Windows 风格换行中的回车；限制单行 4096 字节，防止无限积累输入。
                } else if (data[i] != '\r') {
                    if (line.size() >= 4096) {
                        log("cli_error", "命令超过4096字节，输入线程停止");
                        return;
                    }
                    line.push_back(data[i]);
                }
            }
        }
    }
    // stdin EOF 不等于服务退出；systemd 使用空输入时服务继续运行，等待信号。
}
} // namespace inspection
