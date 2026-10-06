/**
 * 文件功能：检测业务的数据模型与状态定义
 *
 * 职责说明：定义产品规则、仪器测量、确认点、任务以及检测流程阶段。
 * 调用关系：配置生成 Rule；仪器生成 Measurement；业务确认后生成 Point，最终汇总到 Task。
 * 阅读提示：厚度整数 150000 表示 150 μm；持久化任务状态与交互流程阶段是两个不同维度。
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace inspection {
// 业务数据模型：不依赖设备句柄。厚度统一为 milli_um（千分之一微米）；
// wall_ms 为 Unix 毫秒，mono_ms 为本次运行的单调时间，二者不可混用。
// 模块一：产品质量规则。每次开始检测复制一份，保证检测期间配置变化不影响历史结果。
struct Rule {
    std::string product_id;  // 配置中的产品型号，创建任务后不得改变。
    std::string version;     // 演示规则版本，随完整规则快照保存。
    std::string substrate;   // 只允许 FE 或 NFE，质量判定时逐点比较。
    int required_points = 5; // 合法范围1～5，每个点必须独立开启并确认。
    // 厚度上下界包含边界，例如 [100000, 200000] 表示 100～200 μm。
    std::int64_t min_milli_um = 100000, max_milli_um = 200000;
};
// 模块二：原始合法测量的业务副本。接收成功并不等于已保存为点位，仍需人工确认。
struct Measurement {
    std::string candidate_id;  // 本机候选UUID，不是仪器内部测量编号。
    std::string instrument_id; // 仪器配置标识，不等同于工件身份。
    std::string substrate;     // 已归一化材料；只有FE/NFE进入候选。
    std::string group;         // 仪器组名，仅作诊断和追溯。
    // 厚度、日历时间和单调时间均用整数保存；后两者分别用于追溯和窗口归属判断。
    std::int64_t thickness_milli_um = 0, wall_ms = 0, mono_ms = 0;
    std::uint64_t generation = 0;     // 成功订阅新连接后递增，用于隔离旧连接。
    std::uint64_t local_sequence = 0; // 本次运行的诊断序号，不用于厚度去重。
};
// 模块三：确认点。index 表示第几个测点，value 保存当时选中的候选，而不是之后的新测量。
struct Point {
    int index = 0; // 从1开始，确认后不可覆盖。
    Measurement value;
    std::int64_t confirmed_ms = 0;
};
// 编译期枚举防止状态字符串拼写错误；数据库使用明确的稳定字符串编码。
// 模块四：任务持久化状态。StartPending 等待 MCU START 确认，Inspecting 正在采样，
// Completed 已提交质量结果，Aborted 因取消/故障终止；中止不等于质量 FAIL。
enum class TaskState { StartPending, Inspecting, Completed, Aborted };
// 交互流程阶段：Idle 空闲；Ready 可开始；StartPending 等待开始确认；
// WaitArm 等待开启采集；WaitNewMeasurement 等待新测量；WaitConfirm 等待人工确认；
// Finalizing 正在提交结果；Completed/Aborted/Fault 分别表示完成、中止、故障。
enum class InspectionPhase {
    Idle,
    Ready,
    StartPending,
    WaitArm,
    WaitNewMeasurement,
    WaitConfirm,
    Finalizing,
    Completed,
    Aborted,
    Fault
};
std::string phase_name(InspectionPhase);
// 模块五：一次检测任务。同一工件复检会创建新的 id，历史检测互不覆盖。
struct Task {
    std::string id;        // 唯一 inspection_id，复检也必须重新分配。
    std::string workpiece; // 人工录入的工件编号，可被多次检测引用。
    std::string result;    // 空/PASS/FAIL；系统中止不填FAIL。
    TaskState state = TaskState::StartPending;
    // 规则快照、现场 token、开始时间和确认点列表共同构成这次检测的上下文。
    Rule rule;
    std::uint32_t token = 0; // MCU使用的非零任务标识，在SQLite持久化关联。
    std::int64_t started_ms = 0;
    std::vector<Point> points;
};
// 质量规则仅应用于已确认点。材料不符合规则属于 FAIL，而不是系统故障。
// 质量判断入口：点数必须恰好满足规则，且每点材料一致、厚度在上下界内。
bool passes(const Rule& rule, const std::vector<Point>& points);
} // namespace inspection
