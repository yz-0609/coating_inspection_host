/**
 * 文件功能：公共运行时工具与质量规则实现
 *
 * 职责说明：实现时钟、UUID、时间可信度、JSON 日志和最终点位质量判断。
 * 调用关系：各模块调用工具；业务完成时调用 passes，数据库完成事务还会独立核对。
 * 阅读提示：计时用单调时钟，审计用日历时钟；日志按行输出 JSON 便于程序解析。
 */
#include "inspection/support.hpp"
#include "inspection/types.hpp"
#include <fstream>
#include <glib.h>
#include <iostream>
namespace inspection {
// 模块一：时钟实现。duration_cast 把时钟原生精度转换为毫秒整数。
std::int64_t SystemClock::mono_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// system_clock 对应日历时间，供日志/数据库记录 Unix 毫秒；校时可能改变其数值。
std::int64_t SystemClock::wall_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
// 模块二：唯一编号。GLib 分配随机 UUID 字符串，复制为 C++ 字符串后用 g_free 释放。
std::string uuid() {
    char* value = g_uuid_string_random();
    std::string result(value);
    g_free(value);
    return result;
}
// 记录系统是否存在同步证据；UNVERIFIED 仍允许存储，但追溯者应知晓时间未验证。
std::string time_quality() {
    // systemd-timesyncd 创建此标志表示已同步；缺少证据时保守标记未验证。
    std::ifstream synchronized("/run/systemd/timesync/synchronized");
    return synchronized.good() ? "SYNCHRONIZED" : "UNVERIFIED";
}
// 模块三：统一日志出口。参数 fields 按值接收，补充公共字段不会改调用方对象。
void log(const std::string& event, const std::string& message, Json fields) {
    // 函数静态互斥量在所有线程之间共享，保证整行日志一起写出。
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    fields["event"] = event;
    fields["message"] = message;
    fields["wall_ms"] = SystemClock{}.wall_ms();
    // dump(-1) 生成紧凑单行 JSON；replace 替换非法 UTF-8，endl 同时换行并刷新输出。
    std::cout << fields.dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
}
// 模块四：阶段名转换。把 C++ 枚举变成稳定的机器可读字符串，供日志展示。
std::string phase_name(InspectionPhase phase) {
    switch (phase) {
    case InspectionPhase::Idle:
        return "IDLE";
    case InspectionPhase::Ready:
        return "READY";
    case InspectionPhase::StartPending:
        return "START_PENDING";
    case InspectionPhase::WaitArm:
        return "WAIT_ARM";
    case InspectionPhase::WaitNewMeasurement:
        return "WAIT_NEW_MEASUREMENT";
    case InspectionPhase::WaitConfirm:
        return "WAIT_CONFIRM";
    case InspectionPhase::Finalizing:
        return "FINALIZING";
    case InspectionPhase::Completed:
        return "COMPLETED";
    case InspectionPhase::Aborted:
        return "ABORTED";
    case InspectionPhase::Fault:
        return "FAULT";
    }
    throw std::logic_error("未知检测状态");
}
// 模块五：质量规则。只判断已确认点；缺点、多点、材料错误或任何一点越界都不通过。
bool passes(const Rule& rule, const std::vector<Point>& points) {
    if (points.size() != static_cast<std::size_t>(rule.required_points))
        return false;
    // 厚度用严格小于/大于判断越界，因此恰好等于下限或上限的点可通过。
    for (const auto& p : points)
        if (p.value.substrate != rule.substrate || p.value.thickness_milli_um < rule.min_milli_um ||
            p.value.thickness_milli_um > rule.max_milli_um)
            return false;
    return true;
}
} // namespace inspection
