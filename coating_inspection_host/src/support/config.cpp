/**
 * 文件功能：YAML 配置读取、校验与路径解析
 *
 * 职责说明：把配置文件转换成经过检查的 Config，尽早拒绝错误配置。
 * 调用关系：main → load_config → 通用字段读取/阈值转换 → 产品规则检查 → 返回配置。
 * 阅读提示：as<T>() 是 YAML 类型转换；异常交给 main 记录，避免服务带着错误参数运行。
 */
// 启动配置：读取 YAML，统一校验边界，路径相对配置文件而非当前工作目录解析。
#include "inspection/config.hpp"
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <yaml-cpp/yaml.h>
namespace inspection {
// 模块一：配置转换辅助函数，仅供本文件使用。
namespace {
// 读取可选字段：段落缺失使用默认值，段落类型错误则拒绝；T 由默认值推导。
template <class T> T field(const YAML::Node& node, const char* key, T fallback) {
    if (!node)
        return fallback;
    if (!node.IsMap())
        throw std::runtime_error(std::string("配置段必须为映射：") + key);
    // 字段存在则按 T 转换，非法文本/类型由 yaml-cpp 抛异常。
    return node[key] ? node[key].as<T>() : fallback;
}
// 阈值最多支持三位小数；拒绝不能用 milli_um 表达的配置，避免静默舍入。
// 质量阈值转换：把 μm 乘 1000 得到 milli_um，要求能精确表示为整数。
std::int64_t thickness(const YAML::Node& value) {
    if (!value)
        throw std::runtime_error("规则缺少厚度阈值");
    double scaled = value.as<double>() * 1000.0;
    // 拒绝 NaN/无穷、超出设备数值范围或超过三位有效小数的输入，不能悄悄改阈值。
    if (!std::isfinite(scaled) || std::abs(scaled) > 2147483647.0 ||
        std::abs(scaled - std::round(scaled)) > 1e-6)
        throw std::runtime_error("厚度阈值必须可用千分之一微米精确表达");
    return static_cast<std::int64_t>(std::llround(scaled));
}
} // namespace
// 模块二：完整配置读取。先读取字段，再验证约束，最终解析文件路径。
Config load_config(const std::string& path) {
    const auto y = YAML::LoadFile(path);
    if (!y.IsMap())
        throw std::runtime_error("配置根节点必须为映射");
    Config c;
    // 保存各 YAML 分段的节点视图，缩短后续读取代码；缺失段可使用默认值。
    auto i = y["instrument"], w = y["workstation"], d = y["database"], cloud = y["cloud"],
         testing = y["testing"];
    // 身份及仪器配置：选择回放/真实 BLE，设置接收日志和回放时序。
    c.station_id = field(y, "station_id", c.station_id);
    c.instrument_type = field(i, "type", c.instrument_type);
    c.device_name = field(i, "device_name", c.device_name);
    c.replay_file = field(i, "replay_file", c.replay_file);
    c.replay_interval_ms = field(i, "interval_ms", c.replay_interval_ms);
    c.replay_loop = field(i, "loop", c.replay_loop);
    c.raw_notifications = field(i, "raw_notifications", false);
    // 工位配置：选择 mock/modbus，并固定真实串口为 8 数据位、偶校验、1 停止位。
    c.workstation_type = field(w, "type", c.workstation_type);
    c.serial_device = field(w, "device", c.serial_device);
    c.baudrate = field(w, "baudrate", c.baudrate);
    c.slave_id = field(w, "slave_id", c.slave_id);
    if (field(w, "parity", std::string("even")) != "even")
        throw std::runtime_error("共同协议要求 even/8E1");
    // 读取工位轮询、心跳、业务健康、通信响应和命令确认的不同期限。
    c.poll_ms = field(w, "poll_ms", c.poll_ms);
    c.heartbeat_ms = field(w, "heartbeat_ms", c.heartbeat_ms);
    c.health_ms = field(w, "business_health_ms", c.health_ms);
    c.response_ms = field(w, "response_timeout_ms", c.response_ms);
    c.command_ms = field(w, "command_timeout_ms", c.command_ms);
    c.retries = field(w, "max_retries", c.retries);
    // 存储、云、测试开关与业务窗口配置；读取只是转换，合法性在下方统一判断。
    c.database_path = field(d, "path", c.database_path);
    c.database_busy_ms = field(d, "busy_timeout_ms", c.database_busy_ms);
    c.disk_min_mb = field(d, "min_free_mb", c.disk_min_mb);
    c.cloud_adapter = field(cloud, "adapter", c.cloud_adapter);
    c.allow_test_controls = field(testing, "allow_controls", false);
    c.measurement_ms = field(y, "measurement_timeout_ms", c.measurement_ms);
    c.queue_capacity = field(y, "queue_capacity", c.queue_capacity);
    // 模块三：后端和参数校验。尚未实现的后端必须报错，不能默默替换成模拟实现。
    if (c.cloud_adapter != "disabled")
        throw std::runtime_error("OneNET 尚未接入：cloud.adapter 必须为 disabled");
    if (c.instrument_type != "ble" && c.instrument_type != "replay")
        throw std::runtime_error("未知仪器后端");
    if (c.workstation_type != "mock" && c.workstation_type != "modbus")
        throw std::runtime_error("未知工位后端");
    // 必需身份非空，队列容量限制在 1～65536，真实 Modbus 必须有设备路径。
    if (c.station_id.empty() || c.database_path.empty() || c.device_name.empty() ||
        c.queue_capacity == 0 || c.queue_capacity > 65536)
        throw std::runtime_error("配置身份、路径或队列容量无效");
    if (c.workstation_type == "modbus" && c.serial_device.empty())
        throw std::runtime_error("缺少串口路径");
    // 交叉检查期限：响应和数据库锁等待不能长于健康期限；心跳频率需留出健康容差；
    // 命令期限至少能容纳一次响应，重试数/从站地址等也要处于合法范围。
    if (c.poll_ms < 10 || c.heartbeat_ms < 10 || c.response_ms < 1 || c.response_ms > c.health_ms ||
        c.health_ms < c.heartbeat_ms * 2 || c.command_ms < c.response_ms || c.retries < 0 ||
        c.retries > 10 || c.measurement_ms < 1 || c.replay_interval_ms < 1 ||
        c.database_busy_ms < 0 || c.database_busy_ms > c.health_ms || c.disk_min_mb < 0 ||
        c.baudrate <= 0 || c.slave_id < 1 || c.slave_id > 247)
        throw std::runtime_error("时间、重试、存储或串口参数无效");
    // 模块四：产品规则读取。至少需要一个产品，逐项建立按型号查找的 map。
    const auto rules = y["products"];
    if (!rules || !rules.IsMap() || rules.size() == 0)
        throw std::runtime_error("必须配置至少一个演示产品");
    // item.first 是产品型号，item.second 是该产品的规则节点。
    for (const auto& item : rules) {
        Rule r;
        r.product_id = item.first.as<std::string>();
        auto v = item.second;
        r.version = field(v, "rule_version", std::string{});
        r.substrate = field(v, "substrate", std::string{});
        r.required_points = field(v, "required_points", 5);
        r.min_milli_um = thickness(v["min_um"]);
        r.max_milli_um = thickness(v["max_um"]);
        // 规则须有版本、1～5 个点、正确上下界及 FE/NFE 材料，否则启动失败。
        if (r.product_id.empty() || r.version.empty() || r.required_points < 1 ||
            r.required_points > 5 || r.min_milli_um > r.max_milli_um ||
            (r.substrate != "FE" && r.substrate != "NFE"))
            throw std::runtime_error("产品规则无效：" + r.product_id);
        c.rules.emplace(r.product_id, r);
    }
    // 路径相对配置文件解析，systemd 和前台运行不会因工作目录不同打开另一数据库。
    // 模块五：文件路径归一化。统一以配置所在目录为基准，便于前台/systemd 得到相同数据。
    const auto base = std::filesystem::absolute(path).parent_path();
    // 空路径保持为空，绝对路径保持原值，相对路径拼接后消除 . 和 ..；不要求目标文件已存在。
    auto resolve = [&base](const std::string& p) {
        return p.empty() ? p
                         : (std::filesystem::path(p).is_absolute()
                                ? p
                                : (base / p).lexically_normal().string());
    };
    c.database_path = resolve(c.database_path);
    c.replay_file = resolve(c.replay_file);
    if (c.instrument_type == "replay" && c.replay_file.empty())
        throw std::runtime_error("缺少回放文件");
    return c;
}
} // namespace inspection
