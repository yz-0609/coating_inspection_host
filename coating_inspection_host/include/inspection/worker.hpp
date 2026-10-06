/**
 * 文件功能：检测状态机与候选展示接口
 *
 * 职责说明：声明业务线程及其状态，集中控制创建任务、采集、确认、完成和故障处理。
 * 调用关系：输入事件 → handle → 各状态处理函数 → 仓库存储/异步工位命令。
 * 阅读提示：外部线程只提供事件；task_、phase_ 和 candidates_ 由业务线程独占。
 */
#pragma once
#include "inspection/cloud.hpp"
#include "inspection/instrument.hpp"
#include "inspection/repository.hpp"
#include "inspection/workstation.hpp"
#include <map>
#include <optional>
#include <thread>
namespace inspection {
// 候选展示绑定：发布日志与更新编号在同一锁中完成；工位投递时复制编号，
// 即使队列内又出现新测量，原按钮仍只确认当时已经展示的候选。
// 模块一：跨线程候选展示绑定。日志与 current_ 更新在同一把锁内，工位线程读取时得到副本。
class CandidateDisplay {
  public:
    // 展示候选并更新可由按钮确认的编号；current 线程安全读取，clear 使旧展示失效。
    void publish(const Task&, const Measurement&);
    std::string current() const;
    void clear();

  private:
    // const current() 也需要加锁，所以 mutex 使用 mutable；这不允许外部修改业务测量。
    mutable std::mutex mutex_;
    std::string current_;
};
// 状态机唯一所有者。对外只有投递、启动和停止，禁止其他线程修改 task_/phase_。
// 模块二：业务线程生命周期。构造保存依赖，start 才真正启动后台执行。
class InspectionWorker {
  public:
    InspectionWorker(Config, const IClock&, BoundedQueue<Event>&, BusinessHealth&,
                     CandidateDisplay&, IInstrument&, IWorkstation&);
    ~InspectionWorker();
    void start();
    void request_stop();
    void join();
    // 主线程可无锁读取完成标志；业务结束后为 true，用于有序退出。
    bool finished() const {
        return finished_.load();
    }
    // 独立记录系统是否发生故障，供 main 决定进程退出码。
    bool failed() const {
        return failed_.load();
    }

  private:
    // 模块三：事件分发与业务流程。run 管理仓库/主循环，handle 按事件类型调用处理函数。
    void run();
    void handle(const Event&);
    void snapshot(const Event&);
    void cli(const Event&);
    // 检测操作：begin 创建任务；arm 建立新窗口；confirm 保存指定候选；abort 中止活动任务。
    void begin(const std::string&, const std::string&);
    void arm();
    void confirm(const std::string&);
    void abort(const std::string&);
    // 现场命令管理：同步分配新 epoch；issue 先记库再排队；command_done 消费 MCU 执行结果。
    void synchronize();
    void issue(Opcode, std::uint32_t argument = 0);
    void command_done(const Event&);
    void status();
    // 故障与退出：fatal 停止健康推进；shutdown 尝试保存中止并等待现场命令的有界收尾。
    void fatal(const std::string&);
    void shutdown();
    bool active() const;
    bool disk_ok() const;
    bool usable_snapshot() const;
    // 模块四：依赖对象。配置持有副本，引用对象由 main 创建，必须比业务线程活得久。
    Config cfg_;
    const IClock& clock_;
    BoundedQueue<Event>& queue_;
    BusinessHealth& health_;
    CandidateDisplay& display_;
    IInstrument& instrument_;
    IWorkstation& workstation_;
    // 仓库在线程内创建/销毁，云对象表示当前未配置状态。
    std::unique_ptr<InspectionRepository> repo_;
    DisabledCloudPublisher cloud_;
    // 线程和可跨线程观察的原子标志；其余业务字段只由 run 所在线程访问。
    std::thread thread_;
    std::atomic_bool stop_requested_{false}, failed_{false}, finished_{false};
    // optional 表示可能没有任务；candidates_ 只保存当前窗口，commands_ 跟踪未收到结果的命令。
    std::optional<Task> task_;
    std::map<std::string, Measurement> candidates_;
    std::map<std::uint32_t, Command> commands_;
    Snapshot snapshot_;
    InspectionPhase phase_ = InspectionPhase::Idle;
    // 一次服务启动的 UUID 与时间质量；epoch_ 区分同步周期，sequence_ 区分周期内命令。
    std::string run_id_, quality_;
    std::uint32_t epoch_ = 0, sequence_ = 0, last_event_ = 0;
    // 重新同步请求可能要等旧命令结束；event_seen_ 与 last_event_ 防止重复处理锁存按钮。
    bool resync_requested_ = false, event_seen_ = false;
    bool instrument_online_ = false, synchronized_ = false, link_online_ = false, fatal_ = false;
    // 当前仪器连接代次与窗口绑定代次；断线重连后旧候选不能用于新窗口。
    std::uint64_t generation_ = 0, window_generation_ = 0;
    // 窗口开启时间、总截止时间和上次磁盘检查时间，均使用单调毫秒。
    std::int64_t window_ms_ = 0, window_deadline_ = 0, last_disk_check_ = 0;
};
} // namespace inspection
