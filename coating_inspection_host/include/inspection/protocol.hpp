/**
 * 文件功能：Linux 与 MCU 的工位协议定义
 *
 * 职责说明：集中声明寄存器地址、命令、状态快照以及编码和确认接口。
 * 调用关系：业务构造 Command → 工位编码发送 → 读取 Snapshot → 验证业务执行结果。
 * 阅读提示：一个 Modbus 寄存器是 16 位；32 位字段使用两个寄存器，先高字后低字。
 */
#pragma once
#include <array>
#include <cstdint>
#include <string>
namespace inspection {
// 共同协议 V2：32 位数始终按高字、低字排列。集中定义避免业务层写裸地址。
// 模块一：协议常量。0x0200 标识 V2；快照 22 个寄存器，心跳与命令各有固定写入地址。
constexpr std::uint16_t protocol_version = 0x0200;
constexpr int snapshot_registers = 22;
constexpr int heartbeat_address = 0x0100, command_address = 0x0110;
// 模块二：MCU 状态编码。从 0 开始依次为启动、空闲、就绪、检测中、完成、故障。
enum class McuState : std::uint16_t { Boot, Idle, Ready, Inspecting, Completed, Fault };
// 业务命令：安全同步、开始检测、应用 PASS/FAIL、确认按钮事件、人工复位、撤销输出。
enum class Opcode : std::uint16_t {
    SyncSafe = 1,
    Start = 2,
    ApplyResult = 3,
    AckInput = 4,
    ResetFault = 5,
    Abort = 6
};
// 模块三：主站命令身份。epoch 隔离主站重启周期，token 关联检测，sequence 关联单条命令，
// argument 是命令参数；重发必须保持这四个字段和 opcode 不变。
struct Command {
    std::uint32_t epoch = 0, token = 0, sequence = 0, argument = 0;
    Opcode opcode = Opcode::SyncSafe;
};
// 模块四：一次工位状态快照。online 是本机通信状态；其余协议字段来自寄存器。
struct Snapshot {
    bool online = false;
    McuState state = McuState::Boot;
    // inputs bit0 为工件到位；outputs bit0/1/2 为 PASS/FAIL/FAULT；
    // fault/health 保存故障和健康码，heartbeat 为 MCU 心跳字段，ack_status=1 表示命令接受。
    std::uint16_t inputs = 0, outputs = 0, fault = 0, health = 0, heartbeat = 0, ack_status = 0;
    std::uint16_t event_code = 0, reset_reason = 0, version = 0;
    // uptime 是 MCU 运行时间，token 为现场任务；ack_epoch/ack_sequence 关联命令确认，
    // event_sequence/event_code 关联锁存按钮事件，errors 为设备诊断累计值。
    std::uint32_t uptime = 0, token = 0, ack_sequence = 0, event_sequence = 0, errors = 0,
                  ack_epoch = 0;
    // 本机轮询时间戳，供业务检查快照是否过期；不是 MCU 寄存器字段。
    std::int64_t mono_ms = 0;
    // 位运算只检查 inputs 的最低位，其他输入位不会影响到位判定。
    bool present() const {
        return (inputs & 1U) != 0;
    }
};
// 模块五：纯协议工具。encode 生成 9 字命令，decode 解析 22 字快照；
// command_satisfied 同时核对 ACK 身份和现场状态，opcode_name 生成稳定日志名称。
std::array<std::uint16_t, 9> encode(const Command& command);
Snapshot decode(const std::array<std::uint16_t, 22>& registers);
bool command_satisfied(const Command&, const Snapshot&);
std::string opcode_name(Opcode);
} // namespace inspection
