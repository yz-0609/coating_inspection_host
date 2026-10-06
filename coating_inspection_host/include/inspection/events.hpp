/**
 * 文件功能：跨线程事件的统一载体
 *
 * 职责说明：把仪器、工位和命令行输入转换成可由业务线程串行消费的消息。
 * 调用关系：生产者构造 Event → EventSink 投递有界队列 → InspectionWorker::handle 分发。
 * 阅读提示：根据 kind 使用对应字段；普通结构体不是 union，各字段有自己的存储空间。
 */
#pragma once
#include "inspection/protocol.hpp"
#include "inspection/types.hpp"
#include <string>
namespace inspection {
// 跨线程事件拥有自己的数据副本。禁止保存原始 BLE 回调指针。
// 模块一：消息类型。依次表示仪器连接、测量、工位快照、命令结果、命令行、停止请求。
enum class EventKind { InstrumentState, Measurement, Snapshot, CommandDone, Cli, Stop };
// 模块二：消息内容。通过值复制/移动跨线程传输，业务线程不会访问回调中的临时指针。
struct Event {
    EventKind kind = EventKind::Cli;
    // 仪器状态用 online/generation；命令结果用 success/detail/command/snapshot。
    bool online = false, success = false;
    std::uint64_t generation = 0;
    // Measurement 类型事件携带完整测量；Snapshot 类型事件携带一次轮询快照。
    Measurement measurement;
    Snapshot snapshot;
    Command command;
    // text 保存命令行；detail 保存执行说明；bound_candidate 在快照投递时绑定已展示候选编号。
    std::string text, detail, bound_candidate;
};
} // namespace inspection
