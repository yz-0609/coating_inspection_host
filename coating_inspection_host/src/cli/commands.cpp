/**
 * 文件功能：命令行命令到业务操作的映射
 *
 * 职责说明：检查参数，调用状态机操作，并提供查询、导出和受配置限制的测试控制。
 * 调用关系：InspectionWorker::handle 收到 Cli 事件 → cli → begin/arm/confirm 等函数。
 * 阅读提示：函数属于 InspectionWorker，即使放在 cli 目录，执行时仍在业务线程。
 */
#include "inspection/cli.hpp"
#include "inspection/worker.hpp"
#include <cmath>
#include <fstream>
#include <sys/resource.h>
namespace inspection {
// 命令处理仍在业务线程执行；拆到 CLI 模块只为保持业务状态机文件职责集中。
// 普通输入错误不会中止任务；StorageError/序列化错误进入不可恢复故障路径。
// 模块一：命令入口。words[0] 是命令名，其他元素是参数；空行无需处理。
void InspectionWorker::cli(const Event& e) {
    auto words = cli_words(e.text);
    if (words.empty())
        return;
    const auto& command = words[0];
    // 语法/操作错误只反馈给用户；数据库、持久化和队列错误必须进入故障路径。
    try {
        // 模块二：信息命令。状态、历史记录、Outbox 查询都在业务线程访问仓库。
        if (command == "status" && words.size() == 1) {
            status();
            return;
        }
        if (command == "query" && words.size() <= 2) {
            log("query", "历史检测记录",
                {{"records", repo_->query(words.size() == 2 ? words[1] : "")}});
            return;
        }
        if (command == "outbox" && words.size() == 1) {
            log("outbox", "待接入云端事件", {{"records", repo_->outbox()}});
            return;
        }
        // 导出内部业务事件 JSON；既检查打开，也检查写入状态，以便报告磁盘/权限错误。
        if (command == "export" && words.size() == 2) {
            std::ofstream file(words[1]);
            if (!file)
                throw std::runtime_error("不能打开导出文件");
            file << repo_->outbox().dump(2) << '\n';
            if (!file)
                throw std::runtime_error("导出写入失败");
            log("export", "事件已导出", {{"path", words[1]}});
            return;
        }
        if (command == "help") {
            log("help", "status | start 工件 产品 | arm | confirm 候选编号 | cancel | reset | "
                        "query [编号] | outbox | export 路径 | quit");
            return;
        }
        // 只发停止请求，真正保存中止和清理在业务 run/shutdown 与 main 中进行。
        if (command == "quit" && words.size() == 1) {
            stop_requested_.store(true);
            return;
        }
        // 模块三：测试控制。allow_test_controls 是统一入口开关，生产环境不可由命令绕过。
        if (command == "sim") {
            if (!cfg_.allow_test_controls)
                throw std::runtime_error("测试控制未启用");
            // 工位到位/按钮/MCU 复位只可控制内存 Mock，真实 PTY 仿真另有控制通道。
            if (words.size() == 3 && words[1] == "present") {
                if (!workstation_.test_control({{"present", words[2] == "1"}}))
                    throw std::runtime_error("该控制只支持 Mock；PTY 模拟器请使用独立控制通道");
                return;
            }
            if (words.size() == 3 && words[1] == "button") {
                if (!workstation_.test_control({{"button", words[2]}}))
                    throw std::runtime_error("按钮仿真只支持 Mock 后端");
                return;
            }
            if (words.size() == 2 && words[1] == "mcu_reset") {
                if (!workstation_.test_control({{"reset_mcu", true}}))
                    throw std::runtime_error("复位仿真只支持 Mock 后端");
                return;
            }
            // 模拟仪器断线/上线，让测试走连接代次切换的真实业务处理。
            if (words.size() == 3 && words[1] == "connection") {
                instrument_.test_control({{"connection", words[2] == "1"}});
                return;
            }
            // 用户输入单位为 μm；检查整串解析、有限值及设备范围，再转整数 milli_um。
            if (words.size() == 4 && words[1] == "measure") {
                std::size_t used = 0;
                double value = std::stod(words[2], &used);
                if (used != words[2].size() || !std::isfinite(value) ||
                    std::abs(value * 1000) > 2147483647.0)
                    throw std::runtime_error("合成厚度无效");
                if (!instrument_.test_control(
                        {{"milli_um", static_cast<std::int64_t>(std::llround(value * 1000))},
                         {"substrate", words[3]}}))
                    throw std::runtime_error("仪器后端不支持测试控制");
                return;
            }
            // 有意阻塞业务线程以测试心跳保护；该操作只对测试进程有意义。
            if (words.size() == 3 && words[1] == "stall") {
                int duration = std::stoi(words[2]);
                if (duration < 0 || duration > 5000)
                    throw std::runtime_error("停滞注入范围0～5000毫秒");
                std::this_thread::sleep_for(std::chrono::milliseconds(duration));
                return;
            }
            if (words.size() == 2 && words[1] == "file_limit") {
                // 只限制当前测试进程。SQLite 的真实文件写入将返回 EFBIG/IOERR。
                struct rlimit limit{};
                limit.rlim_cur = 0;
                limit.rlim_max = 0;
                if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
                    throw std::runtime_error("无法设置测试文件限制");
                log("file_limit_enabled", "当前测试进程文件写入容量已限制为0");
                return;
            }
            // 直接进入与队列溢出相同的故障路径；用于检查业务故障行为。
            if (words.size() == 2 && words[1] == "overflow") {
                fatal("event_queue_overflow");
                return;
            }
            // 将引号中的十六进制字节交给回放解析器，可注入半帧或 CRC 错误。
            if (words.size() == 3 && words[1] == "raw") {
                instrument_.test_control({{"hex", words[2]}});
                return;
            }
            throw std::runtime_error("未知仿真命令");
        }
        // 模块四：检测操作。这里只匹配语法，具体状态前置条件在 begin/arm/confirm 中检查。
        if (command == "start" && words.size() == 3) {
            begin(words[1], words[2]);
            return;
        }
        if (command == "arm" && words.size() == 1) {
            arm();
            return;
        }
        if (command == "confirm" && words.size() == 2) {
            confirm(words[1]);
            return;
        }
        if (command == "cancel" && words.size() == 1) {
            abort("operator_cancel");
            return;
        }
        // 复位有两步：必要时先安全同步，成功后人工再次 reset；活动检测不能被 reset 静默取消。
        if (command == "reset" && words.size() == 1) {
            if (!fatal_ && !active() && !synchronized_ && usable_snapshot() && commands_.empty()) {
                synchronize();
                log("sync_requested", "重新请求安全同步，确认成功后再次 reset");
                return;
            }
            // 只有非致命故障、无活动任务、同步成功、快照新鲜、没有待确认命令且 MCU 故障时才能复位。
            if (fatal_ || active() || !synchronized_ || !usable_snapshot() || !commands_.empty() ||
                snapshot_.state != McuState::Fault)
                throw std::runtime_error("复位前置条件未满足");
            issue(Opcode::ResetFault);
            return;
        }
        throw std::runtime_error("未知命令或参数数量错误，输入 help 查看用法");
    // 模块五：错误分级。存储或 JSON 异常影响业务可靠性，因此触发 fatal。
    } catch (const StorageError& ex) {
        fatal(std::string("database_failure: ") + ex.what());
    } catch (const Json::exception& ex) {
        fatal(std::string("business_serialization_failure: ") + ex.what());
    // 普通语法/操作错误记录后返回，操作者可以改正参数继续操作。
    } catch (const std::exception& ex) {
        log("cli_error", ex.what());
    }
}
} // namespace inspection
