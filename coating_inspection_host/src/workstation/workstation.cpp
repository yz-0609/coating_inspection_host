/**
 * 文件功能：工位通信线程、真实串口和内存模拟器
 *
 * 职责说明：独占设备通信，轮询快照，发送健康心跳，串行发送及重试业务命令。
 * 调用关系：业务 submit → 工位命令队列 → Transport → MCU/Mock → Snapshot/CommandDone 事件。
 * 阅读提示：先看 Transport 接口，再看两个后端，最后看 WorkstationWorker 的调度循环。
 */
// 工位通信实现：Transport 只在工位线程使用；业务命令串行确认，变化心跳独立调度。
// 串口错误意味着现场执行可能未知，必须关闭通道并由业务层重新安全同步。
#include "inspection/workstation.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <modbus/modbus.h>
#include <optional>
#include <thread>
namespace inspection {
namespace {
// 模块一：底层传输接口。仅工位线程调用，封装连接、读快照、写心跳和写命令。
class Transport {
  public:
    virtual ~Transport() = default;
    virtual void connect() = 0;
    virtual void close() = 0;
    virtual Snapshot read() = 0;
    virtual void heartbeat(std::uint16_t) = 0;
    virtual void command(const Command&) = 0;
    // 真实传输默认拒绝测试控制；Mock 覆盖此函数实现内存状态注入。
    virtual bool control(const Json&) {
        return false;
    }
};
// 真实串口实现：所有调用只发生在 WorkstationWorker，构造阶段不打开设备。
// 模块二：真实 RTU 传输。libmodbus 负责请求编码/CRC 和响应，业务 ACK 另行轮询。
class ModbusTransport final : public Transport {
  public:
    // 构造只保存配置，设备连接放在通信线程，避免主线程启动时阻塞打开串口。
    explicit ModbusTransport(const Config& cfg) : cfg_(cfg) {
    }
    // 析构释放串口及 libmodbus 上下文，close 可重复调用。
    ~ModbusTransport() override {
        close();
    }
    // 先清理旧连接，再按配置创建 8E1 RTU 主站并指定从站地址。
    void connect() override {
        close();
        ctx_ = modbus_new_rtu(cfg_.serial_device.c_str(), cfg_.baudrate, 'E', 8, 1);
        if (!ctx_)
            fail();
        modbus_set_slave(ctx_, cfg_.slave_id);
        // 关闭库内部自动恢复，把断线和重连统一交给工位调度；配置期限拆成秒与微秒。
        modbus_set_error_recovery(ctx_, MODBUS_ERROR_RECOVERY_NONE);
        modbus_set_response_timeout(ctx_, static_cast<std::uint32_t>(cfg_.response_ms / 1000),
                                    static_cast<std::uint32_t>((cfg_.response_ms % 1000) * 1000));
        // 禁用逐字节独立期限，响应整体期限由 response_timeout 控制。
        modbus_set_byte_timeout(ctx_, 0, 0);
        if (modbus_connect(ctx_) < 0)
            fail();
    }
    // 先关串口，再 free 库句柄，置 nullptr 供 guard 检查。
    void close() override {
        if (ctx_) {
            modbus_close(ctx_);
            modbus_free(ctx_);
            ctx_ = nullptr;
        }
    }
    // 功能码 04 从输入寄存器地址 0 连读 22 字；必须得到完整数量再 decode。
    Snapshot read() override {
        guard();
        std::array<std::uint16_t, 22> data{};
        int result = modbus_read_input_registers(ctx_, 0, 22, data.data());
        finished();
        if (result != 22)
            fail();
        return decode(data);
    }
    // 功能码 06 写一个保持寄存器；数值变化才证明业务线程有持续推进。
    void heartbeat(std::uint16_t value) override {
        guard();
        int result = modbus_write_register(ctx_, heartbeat_address, value);
        finished();
        if (result != 1)
            fail();
    }
    // 功能码 16 一次写 9 个保持寄存器，确保整条命令一起交付；写成功仅为通信响应。
    void command(const Command& c) override {
        guard();
        auto data = encode(c);
        int result = modbus_write_registers(ctx_, command_address, 9, data.data());
        finished();
        if (result != 9)
            fail();
    }

  private:
    // 将系统 errno 解释为 Modbus 错误并抛异常，由上层关闭连接/报告执行未知。
    [[noreturn]] void fail() {
        throw std::runtime_error(std::string("Modbus：") + modbus_strerror(errno));
    }
    // 每个请求前检查句柄，并等待上一事务结束后至少 1750 μs；物理总线时序仍需实机验证。
    void guard() {
        if (!ctx_)
            throw std::runtime_error("串口未连接");
        std::this_thread::sleep_until(last_end_ + std::chrono::microseconds(1750));
    }
    // 记录事务结束的单调时刻，供下一次请求间隔计算。
    void finished() {
        last_end_ = std::chrono::steady_clock::now();
    }
    Config cfg_;
    modbus_t* ctx_ = nullptr;
    std::chrono::steady_clock::time_point last_end_{};
};
// 内存工位用于业务测试；独立 PTY 模拟器用于验证真实主站，二者不混用。
// 模块三：内存 MCU 行为模型。它验证业务顺序，不模拟串口电气或 libmodbus 的字节协议。
class MockTransport final : public Transport {
  public:
    // 上电默认故障安全态，需安全同步+人工复位；设置正确协议版本和健康位。
    explicit MockTransport(const IClock& clock) : clock_(clock) {
        s_.online = true;
        s_.version = protocol_version;
        s_.state = McuState::Fault;
        s_.fault = 6;
        // outputs=4 表示只亮 FAULT（bit2）；1 表示 PASS，2 表示 FAIL，0 表示全清。
        s_.outputs = 4;
        s_.health = 3;
        started_ = clock.mono_ms();
    }
    void connect() override {
        connected_ = true;
    }
    void close() override {
        connected_ = false;
    }
    Snapshot read() override {
        // 读快照先运行保护逻辑，然后复制当前状态；在线值来自模拟连接开关。
        tick();
        auto s = s_;
        s.online = connected_;
        s.uptime = static_cast<std::uint32_t>(clock_.mono_ms() - started_);
        return s;
    }
    void heartbeat(std::uint16_t value) override {
        // 相同心跳值不能刷新健康时间，防止通信线程重复发送旧值掩盖业务停滞。
        if (value != last_heartbeat_) {
            last_heartbeat_ = value;
            last_health_ = clock_.mono_ms();
        }
        s_.heartbeat++;
    }
    void command(const Command& c) override {
        tick();
        // 模拟幂等处理：同 epoch/sequence 的重复命令必须内容一致，返回缓存 ACK，不重复执行动作。
        auto words = encode(c);
        if (have_command_ && c.epoch == last_command_.epoch &&
            c.sequence == last_command_.sequence) {
            s_.ack_epoch = c.epoch;
            s_.ack_sequence = c.sequence;
            s_.ack_status = words == encode(last_command_) ? last_status_ : 4;
            return;
        }
        // ACK 码：1 成功、2 状态不允许、3 token 无效、4 身份/序号冲突、5 健康失败、6 参数无效。
        std::uint16_t status = 1;
        // 普通命令必须属于当前 epoch 且序号前进；新同步才能建立更大的主站周期。
        if (c.opcode != Opcode::SyncSafe && (c.epoch != epoch_ || c.sequence <= last_sequence_))
            status = 4;
        // 安全同步清任务/按钮/输出，保持故障安全态，等待人工 ResetFault。
        else if (c.opcode == Opcode::SyncSafe) {
            if (!c.epoch || c.epoch <= epoch_)
                status = 4;
            else {
                epoch_ = c.epoch;
                last_sequence_ = 0;
                have_command_ = false;
                s_.token = 0;
                s_.event_code = 0;
                s_.state = McuState::Fault;
                s_.outputs = 4;
                s_.fault = 6;
            }
        // 只确认指定 event_sequence，不能误清随后出现的新按钮事件。
        } else if (c.opcode == Opcode::AckInput) {
            if (c.argument != s_.event_sequence || s_.event_code == 0)
                status = 6;
            else
                s_.event_code = 0;
        // 人工复位要求心跳新鲜和健康；按工件到位选择 Ready 或 Idle。
        } else if (c.opcode == Opcode::ResetFault) {
            if (!fresh() || !s_.health)
                status = 5;
            else {
                s_.state = s_.present() ? McuState::Ready : McuState::Idle;
                s_.outputs = 0;
                s_.fault = 0;
                s_.token = 0;
            }
        // START 要求就绪、到位、非零 token、健康；成功后绑定 token 并进入 Inspecting。
        } else if (c.opcode == Opcode::Start) {
            if (s_.state != McuState::Ready || !s_.present())
                status = 2;
            else if (!c.token)
                status = 3;
            else if (!fresh() || !s_.health)
                status = 5;
            else {
                s_.state = McuState::Inspecting;
                s_.token = c.token;
                s_.outputs = 0;
            }
        // 结果命令只允许同一检测 token 的 PASS/FAIL；条件不满足返回拒绝，不改变成功状态。
        } else if (c.opcode == Opcode::ApplyResult) {
            if (s_.state != McuState::Inspecting)
                status = 2;
            else if (s_.token != c.token)
                status = 3;
            else if (c.argument != 1 && c.argument != 2)
                status = 6;
            else if (!fresh())
                status = 5;
            else {
                s_.state = McuState::Completed;
                s_.outputs = c.argument == 1 ? 1 : 2;
            }
        // 撤销不能影响另一个 token 的任务；成功进入 HOST_ABORT 故障并清 token。
        } else if (c.opcode == Opcode::Abort) {
            if (s_.token && s_.token != c.token)
                status = 3;
            else {
                s_.state = McuState::Fault;
                s_.outputs = 4;
                s_.fault = 5;
                s_.token = 0;
            }
        } else
            status = 6;
        // 总是回写本次 ACK 身份和状态，让主站知道是哪条命令成功或被拒绝。
        s_.ack_epoch = c.epoch;
        s_.ack_sequence = c.sequence;
        s_.ack_status = status;
        if (c.epoch == epoch_) {
            last_sequence_ = std::max(last_sequence_, c.sequence);
            last_command_ = c;
            last_status_ = status;
            have_command_ = true;
        }
    }
    // 模块四：Mock 测试控制。只在工位线程解释 JSON，外部线程通过队列提交。
    bool control(const Json& j) override {
        if (j.contains("present")) {
            bool present = j.at("present").get<bool>();
            s_.inputs = present ? 1 : 0;
            // 检测中离位会故障；完成后正常离位会清结果回到 Idle。
            if (!present && (s_.state == McuState::Inspecting)) {
                s_.state = McuState::Fault;
                s_.fault = 2;
                s_.outputs = 4;
                s_.token = 0;
            } else if (!present && s_.state == McuState::Completed) {
                s_.state = McuState::Idle;
                s_.outputs = 0;
                s_.token = 0;
            } else if (s_.state == McuState::Idle || s_.state == McuState::Ready)
                s_.state = present ? McuState::Ready : McuState::Idle;
        }
        // 按钮采用锁存模型，未确认时再次按按钮累加 errors，避免无声覆盖旧事件。
        if (j.contains("button")) {
            if (s_.event_code)
                ++s_.errors;
            else {
                ++s_.event_sequence;
                s_.event_code = j.at("button") == "action" ? 1 : 2;
            }
        }
        // 模拟 MCU 重启：清 epoch/ACK/token，主站可通过运行时间或 ACK 周期变化识别重启。
        if (j.value("reset_mcu", false)) {
            s_.state = McuState::Fault;
            s_.token = 0;
            s_.outputs = 4;
            s_.fault = 6;
            s_.ack_epoch = 0;
            s_.ack_sequence = 0;
            s_.ack_status = 0;
            epoch_ = 0;
            last_sequence_ = 0;
            have_command_ = false;
            started_ = clock_.mono_ms();
        }
        return true;
    }

  private:
    // Mock 将变化心跳 1500 ms 内视为新鲜；该值是模拟器保护期限，不是 cfg.health_ms。
    bool fresh() const {
        return last_health_ >= 0 && clock_.mono_ms() - last_health_ <= 1500;
    }
    // 建立 epoch 后心跳超时即清任务并故障；物理 MCU 应实现等价独立保护。
    void tick() {
        if (epoch_ && !fresh()) {
            s_.state = McuState::Fault;
            s_.fault = 1;
            s_.outputs = 4;
            s_.token = 0;
        }
    }
    const IClock& clock_;
    Snapshot s_;
    bool connected_ = false, have_command_ = false;
    std::int64_t started_ = 0, last_health_ = -1;
    std::uint16_t last_heartbeat_ = 0, last_status_ = 0;
    std::uint32_t epoch_ = 0, last_sequence_ = 0;
    Command last_command_;
};
// 模块五：工位线程调度器。负责命令串行化、心跳、轮询和连接退避。
class WorkstationWorker final : public IWorkstation {
  public:
    // 根据已校验配置选择 ModbusTransport 或 MockTransport，二者共享后续调度逻辑。
    WorkstationWorker(const Config& cfg, const IClock& clock, BusinessHealth& health,
                      EventSink sink)
        : cfg_(cfg), clock_(clock), health_(health), sink_(std::move(sink)),
          transport_(cfg.workstation_type == "modbus"
                         ? std::unique_ptr<Transport>(std::make_unique<ModbusTransport>(cfg))
                         : std::unique_ptr<Transport>(std::make_unique<MockTransport>(clock))) {
    }
    // 析构等待通信停止，保证 transport 与事件出口不再被后台线程使用。
    ~WorkstationWorker() override {
        stop();
    }
    // 启动本对象独占的通信线程；只有 run 调用 transport_ 的读写。
    void start() override {
        thread_ = std::thread([this] { run(); });
    }
    // 原子请求停止后 join；有界通信调用完成后循环才可退出。
    void stop() override {
        stopped_.store(true);
        if (thread_.joinable())
            thread_.join();
    }
    // 业务命令队列最多 8 条，持锁只做入队并立即返回，避免阻塞业务健康推进。
    bool submit(Command c) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || commands_.size() >= 8)
            return false;
        commands_.push_back(c);
        return true;
    }
    // 仅 mock 接受控制，最多 128 条；状态修改在 run 消费后执行。
    bool test_control(const Json& j) override {
        if (cfg_.workstation_type != "mock")
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (controls_.size() >= 128)
            return false;
        controls_.push_back(j);
        return true;
    }

  private:
    // 模块六：结果事件封装。携带原始命令身份和验证所用快照，供业务关联动作记录。
    void done(const Command& c, bool success, const std::string& detail, const Snapshot& snapshot) {
        Event e;
        e.kind = EventKind::CommandDone;
        e.command = c;
        e.success = success;
        e.detail = detail;
        e.snapshot = snapshot;
        sink_(std::move(e));
    }
    // 快照投递时记录本机单调时间；Event 持有数据副本，工位下一轮修改不影响队列消息。
    void publish(Snapshot s) {
        s.mono_ms = clock_.mono_ms();
        Event e;
        e.kind = EventKind::Snapshot;
        e.snapshot = s;
        sink_(std::move(e));
    }
    // 模块七：通信主循环。pending 最多一条，后续命令排队，确保 ACK 不被并发命令覆盖。
    void run() {
        bool connected = false;
        // 分别调度重连、读快照、写心跳；deadline 控制单条业务命令的整体等待期限。
        std::int64_t next_connect = 0, next_poll = 0, next_heartbeat = 0;
        unsigned reconnect = 0;
        std::uint16_t heartbeat = 0;
        std::optional<Command> pending;
        std::int64_t deadline = 0, next_retry = 0;
        int transmissions = 0;
        Snapshot snapshot;
        while (!stopped_.load()) {
            auto now = clock_.mono_ms();
            try {
                // 断线状态按退避时刻尝试连接；等待中短暂睡眠，仍周期检查停止请求。
                if (!connected) {
                    if (now < next_connect) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    transport_->connect();
                    connected = true;
                    reconnect = 0;
                    next_poll = next_heartbeat = 0;
                }
                std::deque<Json> controls;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 锁内取走测试控制并取一条待处理命令，锁外才做通信，防止串口等待卡住 submit。
                    controls.swap(controls_);
                    if (!pending && !commands_.empty()) {
                        pending = commands_.front();
                        commands_.pop_front();
                        // 总期限从命令被取出开始计时；重试始终使用同一 pending，不分配新序号。
                        deadline = now + cfg_.command_ms;
                        next_retry = now;
                        transmissions = 0;
                    }
                }
                for (const auto& control : controls)
                    transport_->control(control);
                // 心跳优先于业务读写；健康戳过期后即使通信正常也不继续刷新计数。
                // 读取业务最近一次正常循环时间；即使串口通信正常，业务过期也不能继续发变化心跳。
                const auto healthy = health_.last_loop_ms.load();
                if (now >= next_heartbeat && snapshot.online &&
                    snapshot.version == protocol_version && health_.enabled.load() &&
                    healthy >= 0 && now - healthy <= cfg_.health_ms) {
                    transport_->heartbeat(++heartbeat);
                    next_heartbeat = clock_.mono_ms() + cfg_.heartbeat_ms;
                }
                // 心跳事务也消耗墙钟时间；重试前重新取单调时钟，不能越过总期限发送。
                now = clock_.mono_ms();
                // 超过总期限先报告 UNKNOWN 类超时，不得在期限之后继续发新重试。
                if (pending && now >= deadline) {
                    done(*pending, false, "business_ack_timeout", snapshot);
                    pending.reset();
                }
                // transmissions 初值 0，允许首次发送加 cfg.retries 次重试；重试间隔按总期限分摊。
                if (pending && now >= next_retry && transmissions <= cfg_.retries) {
                    transport_->command(*pending);
                    ++transmissions;
                    next_retry = now + std::max(1, cfg_.command_ms / (cfg_.retries + 1));
                    log("command_transmit", "业务命令已发送，等待工位确认",
                        {{"host_epoch", pending->epoch},
                         {"command_seq", pending->sequence},
                         {"opcode", opcode_name(pending->opcode)},
                         {"attempt", transmissions}});
                }
                now = clock_.mono_ms();
                // 按 poll_ms 读完整快照，既供业务观察，也供当前命令匹配 ACK。
                if (now >= next_poll) {
                    snapshot = transport_->read();
                    snapshot.mono_ms = clock_.mono_ms();
                    publish(snapshot);
                    next_poll = snapshot.mono_ms + cfg_.poll_ms;
                }
                if (pending) {
                    // ACK 必须属于 pending 的 epoch/sequence，非零且采样时刻不晚于截止时间；
                    // 再调用 command_satisfied 核对现场状态，旧 ACK 或仅成功写响应都不能当作执行成功。
                    if (snapshot.ack_epoch == pending->epoch &&
                        snapshot.ack_sequence == pending->sequence && snapshot.ack_status &&
                        snapshot.mono_ms <= deadline) {
                        const bool success = command_satisfied(*pending, snapshot);
                        done(*pending, success,
                             success ? "executed" : "ack_rejected_or_state_mismatch", snapshot);
                        pending.reset();
                    } else if (clock_.mono_ms() >= deadline) {
                        done(*pending, false, "business_ack_timeout", snapshot);
                        pending.reset();
                    }
                }
            } catch (const std::exception& e) {
                // 写响应丢失可能意味着命令已执行；关闭通道后报告 UNKNOWN，不能偷偷换序号重试。
                // 模块八：传输错误恢复。标离线并报告 pending 未知；排队未发送命令逐一报告离线失败。
                transport_->close();
                connected = false;
                snapshot.online = false;
                publish(snapshot);
                if (pending) {
                    done(*pending, false, std::string("transport_unknown: ") + e.what(), snapshot);
                    pending.reset();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    while (!commands_.empty()) {
                        done(commands_.front(), false, "transport_offline", snapshot);
                        commands_.pop_front();
                    }
                }
                // 连续连接失败按 1/2/4/8/10 秒退避，连接成功后重置档位。
                const int delays[] = {1, 2, 4, 8, 10};
                next_connect = clock_.mono_ms() + delays[std::min(reconnect++, 4U)] * 1000;
                log("workstation_error", e.what());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // 线程退出时给当前未确认命令失败事件，最后关闭设备；主程序此前会尽量等待业务收尾。
        if (pending)
            done(*pending, false, "service_stopping", snapshot);
        transport_->close();
    }
    Config cfg_;
    const IClock& clock_;
    BusinessHealth& health_;
    EventSink sink_;
    // 模块九：私有状态。transport_ 单线程独占，命令/控制队列受 mutex_ 保护，停止用原子变量。
    std::unique_ptr<Transport> transport_;
    std::mutex mutex_;
    std::deque<Command> commands_;
    std::deque<Json> controls_;
    std::atomic_bool stopped_{false};
    std::thread thread_;
};
} // namespace
// 模块十：统一工厂入口。外部只见 IWorkstation，具体线程与传输实现隐藏于本文件。
std::unique_ptr<IWorkstation> make_workstation(const Config& cfg, const IClock& clock,
                                               BusinessHealth& health, EventSink sink) {
    return std::make_unique<WorkstationWorker>(cfg, clock, health, std::move(sink));
}
} // namespace inspection
