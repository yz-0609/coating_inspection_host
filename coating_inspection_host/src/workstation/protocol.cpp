/**
 * 文件功能：工位寄存器编解码和业务确认检查
 *
 * 职责说明：在 Command/Snapshot 与寄存器数组之间转换，并核对命令真正执行后的状态。
 * 调用关系：WorkstationWorker 通过 encode/decode 通信，通过 command_satisfied 判断结果。
 * 阅读提示：写寄存器响应只说明通信完成；必须匹配 epoch、序号、ACK 和预期状态。
 */
// 协议编解码与 ACK 验证：统一高低字顺序；确认既检查历史 ACK，也检查当前工位状态。
#include "inspection/protocol.hpp"
#include <stdexcept>
namespace inspection {
// 模块一：高低字工具，仅用于工位协议。
namespace {
// 先把高字提升到 32 位再左移 16 位，避免在窄类型中移位溢出。
std::uint32_t joined(std::uint16_t high, std::uint16_t low) {
    return (static_cast<std::uint32_t>(high) << 16U) | low;
}
// 右移得到高 16 位，窄化转换保留低 16 位；这里处理寄存器字序而不是机器内存字节序。
void split(std::uint32_t value, std::uint16_t& high, std::uint16_t& low) {
    high = static_cast<std::uint16_t>(value >> 16U);
    low = static_cast<std::uint16_t>(value);
}
} // namespace
// 模块二：命令编码。寄存器顺序为 epoch高/低、token高/低、sequence高/低、opcode、argument高/低。
std::array<std::uint16_t, 9> encode(const Command& c) {
    std::array<std::uint16_t, 9> r{};
    split(c.epoch, r[0], r[1]);
    split(c.token, r[2], r[3]);
    split(c.sequence, r[4], r[5]);
    r[6] = static_cast<std::uint16_t>(c.opcode);
    split(c.argument, r[7], r[8]);
    return r;
}
// 模块三：快照解码。只有状态值 0～5 合法；未知状态使通信调用进入错误路径。
Snapshot decode(const std::array<std::uint16_t, 22>& r) {
    if (r[0] > 5)
        throw std::runtime_error("MCU 状态编码未知");
    Snapshot s;
    s.online = true;
    s.state = static_cast<McuState>(r[0]);
    // 寄存器 1～5 对应输入、输出、故障、健康和 MCU 心跳。
    s.inputs = r[1];
    s.outputs = r[2];
    s.fault = r[3];
    s.health = r[4];
    s.heartbeat = r[5];
    // 寄存器 6～11 是运行时间、任务 token 和 ACK 序号，每项按高低字合并。
    s.uptime = joined(r[6], r[7]);
    s.token = joined(r[8], r[9]);
    s.ack_sequence = joined(r[10], r[11]);
    // 寄存器 12 为 ACK 状态；13～18 为按钮序号/类型、复位原因和累计错误。
    s.ack_status = r[12];
    s.event_sequence = joined(r[13], r[14]);
    s.event_code = r[15];
    s.reset_reason = r[16];
    s.errors = joined(r[17], r[18]);
    // 19 为版本，20～21 为 ACK 的主站 epoch，用来识别服务重启前的旧确认。
    s.version = r[19];
    s.ack_epoch = joined(r[20], r[21]);
    return s;
}
// 模块四：业务执行确认。先匹配版本、epoch、序号和成功 ACK，再验证该命令的后置条件。
bool command_satisfied(const Command& c, const Snapshot& s) {
    if (!s.online || s.version != protocol_version || s.ack_epoch != c.epoch ||
        s.ack_sequence != c.sequence || s.ack_status != 1)
        return false;
    switch (c.opcode) {
    // 安全同步后必须在故障安全态，token 清零，只有 FAULT 输出（bit2=4）。
    case Opcode::SyncSafe:
        return s.state == McuState::Fault && s.token == 0 && s.outputs == 4;
    // 开始确认要求检测状态、同一任务 token、到位保持及结果输出清零。
    case Opcode::Start:
        return s.state == McuState::Inspecting && s.token == c.token && s.present() &&
               s.outputs == 0;
    // 完成确认要求同一任务且仍到位，输出与请求 PASS=1 / FAIL=2 一致。
    case Opcode::ApplyResult:
        return s.state == McuState::Completed && s.token == c.token && s.present() &&
               s.outputs == (c.argument == 1 ? 1 : 2);
    // 人工复位后处于 Ready/Idle，任务 token 和输出清零。
    case Opcode::ResetFault:
        return (s.state == McuState::Ready || s.state == McuState::Idle) && s.token == 0 &&
               s.outputs == 0;
    // 撤销后必须进入故障、清任务并输出 FAULT；只看到 ACK=1 还不够。
    case Opcode::Abort:
        return s.state == McuState::Fault && s.outputs == 4 && s.token == 0;
    // 按钮确认成功应清除锁存 event_code，避免同一按钮被持续重复处理。
    case Opcode::AckInput:
        return s.event_code == 0;
    }
    return false;
}
// 模块五：协议名称映射，用于日志和动作记录，未知枚举保守返回 UNKNOWN。
std::string opcode_name(Opcode op) {
    switch (op) {
    case Opcode::SyncSafe:
        return "SYNC_SAFE";
    case Opcode::Start:
        return "START";
    case Opcode::ApplyResult:
        return "APPLY_RESULT";
    case Opcode::AckInput:
        return "ACK_INPUT_EVENT";
    case Opcode::ResetFault:
        return "RESET_FAULT";
    case Opcode::Abort:
        return "ABORT";
    }
    return "UNKNOWN";
}
} // namespace inspection
