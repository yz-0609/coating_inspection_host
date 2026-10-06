/**
 * 文件功能：BLE 与回放仪器的业务适配
 *
 * 职责说明：把厂商通知转换为自有测量事件，管理连接代次、接收时间与采集线程。
 * 调用关系：make_instrument 选择后端 → 原始帧解析 → InstrumentEvents → 业务事件队列。
 * 阅读提示：BLE 的解析由 BluezReader 完成；回放走本文件的 FrameParser，二者共用厂商协议算法。
 */
// 仪器模块：BLE 与回放共享数据事件语义，均使用上游协议解析器。
// 回调参数只在回调期间有效，投递到业务队列的测量必须拥有数据副本。
#include "inspection/instrument.hpp"
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thickness/bluez_reader.hpp>
#include <thread>
namespace inspection {
// 本文件内部实现：辅助转换和具体后端不对外暴露，外部只通过 IInstrument 操作。
namespace {
// 模块一：回放字节转换。按空白切分十六进制 token，要求每个 token 完整且在 0～255。
std::vector<std::uint8_t> bytes(const std::string& hex) {
    std::istringstream input(hex);
    std::string token;
    std::vector<std::uint8_t> result;
    while (input >> token) {
        std::size_t end = 0;
        auto value = std::stoul(token, &end, 16);
        if (end != token.size() || value > 255)
            throw std::runtime_error("回放十六进制字节无效");
        result.push_back(static_cast<std::uint8_t>(value));
    }
    // 限制一条通知的长度，避免异常回放文件产生过大内存占用。
    if (result.size() > 4096)
        throw std::runtime_error("单次回放通知超过4096字节");
    return result;
}
// 将原始字节格式化为两位十六进制供日志观察，不参与解析和质量判断。
std::string hex_string(const std::uint8_t* data, std::size_t size) {
    std::ostringstream s;
    s << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i)
        s << std::setw(2) << static_cast<unsigned>(data[i]) << ' ';
    return s.str();
}
// 两个后端共享事件转换，不复制厂商解析算法。连接切换时清除旧半帧。
// 模块二：共享业务事件转换。成员仅由对应采集线程串行使用，无需跨线程直接修改。
class InstrumentEvents {
  public:
    InstrumentEvents(const Config& cfg, const IClock& clock, EventSink sink)
        : cfg_(cfg), clock_(clock), sink_(std::move(sink)) {
    }
    // 连接变化清空回放解析器；每次上线递增 generation，让业务隔离旧连接数据。
    void connection(bool online) {
        parser_.reset();
        online_ = online;
        if (online)
            ++generation_;
        Event e;
        e.kind = EventKind::InstrumentState;
        e.online = online;
        e.generation = generation_;
        sink_(std::move(e));
    }
    // 接收通知入口：可打印原始字节，并在解析前记录实际接收时间。
    void notification(const std::uint8_t* data, std::size_t size) {
        if (cfg_.raw_notifications)
            log("raw_notification", "原始仪器通知",
                {{"hex", hex_string(data, size)}, {"generation", generation_}});
        if (!online_)
            return;
        // 接收时间在回调入口采样，不能等业务线程消费时再取时间。
        received_wall_ = clock_.wall_ms();
        received_mono_ = clock_.mono_ms();
    }
    // 为厂商测量建立业务副本：新候选 UUID、仪器身份、整数厚度、材料、时钟和连接代次。
    void measurement(const thickness::Measurement& value) {
        if (!online_)
            return;
        Event e;
        e.kind = EventKind::Measurement;
        auto& m = e.measurement;
        m.candidate_id = uuid();
        m.instrument_id = cfg_.device_name;
        m.thickness_milli_um = value.milli_um;
        m.substrate = std::string(thickness::material_name(value.material));
        m.group = value.group;
        m.wall_ms = received_wall_;
        m.mono_ms = received_mono_;
        m.generation = generation_;
        // 本次运行的采集序号仅供诊断，不能把相同厚度或重复序号当作确认点身份。
        m.local_sequence = ++sequence_;
        sink_(std::move(e));
    }
    // 报告本次错误与累计错误；错误帧不会生成候选，便于发现传输质量问题。
    void diagnostic(std::size_t crc, std::size_t protocol) {
        crc_errors_ += crc;
        protocol_errors_ += protocol;
        log("instrument_diagnostic", "无效仪器帧已丢弃",
            {{"crc_errors", crc},
             {"protocol_errors", protocol},
             {"total_crc_errors", crc_errors_},
             {"total_protocol_errors", protocol_errors_}});
    }
    // 回放数据经过同一个厂商 FrameParser；一次通知可解析出多个独立测量。
    void feed(const std::vector<std::uint8_t>& data) {
        notification(data.data(), data.size());
        if (!online_)
            return;
        auto r = parser_.feed(data);
        for (const auto& m : r.measurements)
            measurement(m);
        if (r.crc_errors || r.protocol_errors)
            diagnostic(r.crc_errors, r.protocol_errors);
    }

  private:
    Config cfg_;
    const IClock& clock_;
    EventSink sink_;
    // 此解析器供回放路径使用；真实 BLE 路径的解析器位于 BluezReader 内部。
    thickness::FrameParser parser_;
    std::size_t crc_errors_ = 0, protocol_errors_ = 0;
    bool online_ = false;
    std::uint64_t generation_ = 0, sequence_ = 0;
    std::int64_t received_wall_ = 0, received_mono_ = 0;
};
// 模块三：真实 BLE 后端。把 ReaderCallbacks 接到共享事件转换，不重写厂商解码算法。
class BleInstrument final : public IInstrument {
  public:
    BleInstrument(const Config& c, const IClock& clock, EventSink sink)
        : events_(c, clock, std::move(sink)) {
        thickness::ReaderConfig rc;
        rc.device = c.device_name;
        thickness::ReaderCallbacks callbacks;
        // 原始通知先打接收时间戳，随后 measurement 回调使用该时间。
        callbacks.notification = [this](const std::uint8_t* data, std::size_t n) {
            events_.notification(data, n);
        };
        callbacks.measurement = [this](const thickness::Measurement& m) { events_.measurement(m); };
        // 只有连接并订阅成功才上线；重连等待或停止时离线，业务收到后会处理代次/中止。
        callbacks.state = [this](thickness::ReaderState state, const std::string& message) {
            if (state == thickness::ReaderState::connected)
                events_.connection(true);
            else if (state == thickness::ReaderState::reconnecting ||
                     state == thickness::ReaderState::stopped)
                events_.connection(false);
            log("ble_state", message);
        };
        callbacks.diagnostic = [this](std::size_t crc, std::size_t protocol) {
            events_.diagnostic(crc, protocol);
        };
        callbacks.error = [](const std::string& message) { log("instrument_diagnostic", message); };
        reader_ = std::make_unique<thickness::BluezReader>(std::move(rc), std::move(callbacks));
    }
    // 析构必须先停止并等待线程，确保 reader/events 在回调结束后才销毁。
    ~BleInstrument() override {
        stop();
    }
    // 创建采集线程运行 BluezReader::run；如果系统 D-Bus 初始化失败，发离线事件和错误日志。
    void start() override {
        thread_ = std::thread([this] {
            if (reader_->run() != 0) {
                events_.connection(false);
                log("ble_error", "BlueZ 初始化失败");
            }
        });
    }
    void stop() override {
        // 设置原子停止标志后 join；同步 D-Bus 调用完成或超时后才能响应退出。
        reader_->request_stop();
        if (thread_.joinable())
            thread_.join();
    }
    // 真实 BLE 不接受测试伪造通知，此入口明确返回 false。
    bool test_control(const Json&) override {
        return false;
    }

  private:
    InstrumentEvents events_;
    std::unique_ptr<thickness::BluezReader> reader_;
    std::thread thread_;
};
// 模块四：回放后端。读取 JSON Lines，每行一个控制项或字节通知，在后台按时序执行。
class ReplayInstrument final : public IInstrument {
  public:
    ReplayInstrument(const Config& c, const IClock& clock, EventSink sink)
        : cfg_(c), events_(c, clock, std::move(sink)) {
        std::ifstream file(c.replay_file);
        if (!file)
            throw std::runtime_error("不能打开回放文件：" + c.replay_file);
        std::string line;
        // 启动阶段先解析并校验整个文件，跳过空行及 # 开头行，避免运行途中才发现明显格式错误。
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            // 提前校验 hex 和非负延迟；connection/milli_um 等控制由 apply 解释。
            auto item = Json::parse(line);
            if (item.contains("hex"))
                bytes(item.at("hex").get<std::string>());
            if (item.value("delay_ms", 0) < 0)
                throw std::runtime_error("回放延迟不能为负数");
            items_.push_back(std::move(item));
        }
        if (items_.empty())
            throw std::runtime_error("回放文件为空");
    }
    ~ReplayInstrument() override {
        stop();
    }
    void start() override {
        thread_ = std::thread([this] { run(); });
    }
    void stop() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 停止回放：在锁内改状态并通知等待者，锁释放后 join 避免阻止线程检查停止标志。
            stopped_ = true;
            wake_.notify_all();
        }
        if (thread_.joinable())
            thread_.join();
    }
    // 将控制复制到最多 128 项的队列，再唤醒采集线程；调用者不直接执行 apply。
    bool test_control(const Json& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || controls_.size() >= 128)
            return false;
        controls_.push_back(value);
        wake_.notify_all();
        return true;
    }

  private:
    // 模块五：回放控制解释。connection 改连接，hex 注入字节，milli_um 生成完整仪器格式。
    void apply(const Json& item) {
        if (item.contains("connection")) {
            events_.connection(item.at("connection").get<bool>());
            return;
        }
        if (item.contains("hex")) {
            events_.feed(bytes(item.at("hex").get<std::string>()));
            return;
        }
        if (item.contains("milli_um")) {
            // 合成仪器帧仍经过原始流式解析器和 CRC，不能绕过验证直接伪造候选事件。
            const auto value = item.at("milli_um").get<std::int64_t>();
            if (value > 2147483647LL || value < -2147483647LL)
                throw std::runtime_error("合成厚度超出仪器格式范围");
            // 厂商格式最高位为符号、其余 31 位为绝对值，不能直接使用补码整数内存布局。
            auto raw = static_cast<std::uint32_t>(value < 0 ? -value : value);
            if (value < 0)
                raw |= 0x80000000U;
            // 合成 26 字节实时帧：帧头 BB、CMD=02、声明长度 24（小端）、厚度、材料、16 字节组名和 CRC。
            std::vector<std::uint8_t> b(26, 0);
            b[0] = 0xBB;
            b[1] = 2;
            b[2] = 24;
            // 将厚度最低字节先写入，严格遵循仪器小端格式。
            for (int i = 0; i < 4; ++i)
                b[static_cast<std::size_t>(4 + i)] = static_cast<std::uint8_t>(raw >> (i * 8));
            const auto mode = item.value("substrate", std::string("FE"));
            b[8] = mode == "FE" ? 1 : (mode == "NFE" ? 0 : 2);
            const std::string group = "Synthetic";
            std::copy(group.begin(), group.end(), b.begin() + 9);
            // CRC 覆盖 CMD/SIZE/DATA 共 24 字节，不包括 BB 帧头和最后的 CRC 字节。
            b[25] = thickness::crc8(b.data() + 1, 24);
            events_.feed(b);
        }
    }
    // 模块六：回放调度。上线后按每项 delay_ms 或默认间隔执行，测试控制可提前唤醒。
    void run() {
        events_.connection(true);
        std::size_t index = 0;
        auto next = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(items_[0].value("delay_ms", cfg_.replay_interval_ms));
        while (true) {
            Json control;
            bool has_control = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // 条件变量等待期间释放 mutex，测试控制和 stop 可安全入队/唤醒。
                wake_.wait_for(lock, std::chrono::milliseconds(10),
                               [this] { return stopped_ || !controls_.empty(); });
                if (stopped_)
                    break;
                if (!controls_.empty()) {
                    control = std::move(controls_.front());
                    controls_.pop_front();
                    has_control = true;
                }
            }
            try {
                // 先处理外部测试项，再检查定时回放；锁已释放，解析和事件投递不会长时间占用控制锁。
                if (has_control)
                    apply(control);
                if (index < items_.size() && std::chrono::steady_clock::now() >= next) {
                    apply(items_[index++]);
                    // 循环开关决定文件读完后是否回到首项；不循环时线程仍能接受控制和停止请求。
                    if (index == items_.size() && cfg_.replay_loop)
                        index = 0;
                    if (index < items_.size())
                        next = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(
                                   items_[index].value("delay_ms", cfg_.replay_interval_ms));
                }
            // 非法控制记录错误并发离线，不能继续把出错数据当合法测量。
            } catch (const std::exception& e) {
                log("replay_error", e.what());
                events_.connection(false);
            }
        }
        events_.connection(false);
    }
    Config cfg_;
    InstrumentEvents events_;
    // items_ 是启动后只读回放表；controls_ 与 stopped_ 在 mutex_ 下由多个线程交换。
    std::vector<Json> items_;
    std::deque<Json> controls_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_ = false;
    std::thread thread_;
};
} // namespace
// 模块七：工厂。配置已在启动时验证，因此 ble 之外的合法选择就是 replay。
std::unique_ptr<IInstrument> make_instrument(const Config& c, const IClock& clock, EventSink sink) {
    if (c.instrument_type == "ble")
        return std::make_unique<BleInstrument>(c, clock, std::move(sink));
    return std::make_unique<ReplayInstrument>(c, clock, std::move(sink));
}
} // namespace inspection
