/**
 * 文件功能：SQLite 持久化仓库接口
 *
 * 职责说明：保存任务、确认点、规则快照、待上传事件和现场命令执行记录。
 * 调用关系：业务线程独占仓库；具体 SQL 和事务管理见 src/persistence/repository.cpp。
 * 阅读提示：事务保证一组修改一起成功或一起回滚；数据库错误用 StorageError 向调用者传播。
 */
#pragma once
#include "inspection/protocol.hpp"
#include "inspection/support.hpp"
#include "inspection/types.hpp"
#include <functional>
#include <sqlite3.h>
namespace inspection {
// 持久化模块：连接由使用它的线程独占；事务结束后才返回成功。
// 查询返回值不携带 sqlite 指针，Statement/Transaction 在实现内负责 RAII 清理。
// 模块一：专用存储异常。继承 runtime_error，允许业务将数据库故障与普通输入错误区分。
class StorageError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
// 模块二：仓库生命周期与操作契约。每个仓库独占一个 SQLite 连接，禁止复制。
class InspectionRepository {
  public:
    // path 可指定数据库文件或测试用 :memory:；busy_ms 控制锁等待，readonly 禁止写入。
    InspectionRepository(const std::string& path, int busy_ms = 100, bool readonly = false);
    // 析构关闭连接；成员函数内部的 SQL 语句在返回前已经释放。
    ~InspectionRepository();
    InspectionRepository(const InspectionRepository&) = delete;
    InspectionRepository& operator=(const InspectionRepository&) = delete;
    // 启动恢复短事务：未完成任务中止；PENDING 现场命令转 UNKNOWN，不恢复输出。
    void recover(std::int64_t now);
    // kind 仅允许 host_epoch/session_token；持久化递增，耗尽时抛出 StorageError。
    std::uint32_t allocate(const std::string& kind);
    // 先写入身份与完整规则快照，初始 START_PENDING；返回即提交成功。
    void create(const Task&, const std::string& time_quality, const std::string& run_id);
    // 仅在 START 业务 ACK 后进入 INSPECTING，非法状态拒绝更新。
    void mark_started(const std::string& id);
    // 顺序插入一个确认点；重复点、重复候选或越界被数据库约束拒绝。
    void save_point(const std::string& id, const Point&);
    // 从持久化点重新核对质量结果，与 Outbox 同事务；失败自动回滚。
    void complete(const Task&, const std::string& station, std::int64_t now);
    // 只中止尚未完成的任务；已经提交的 PASS/FAIL 永远不被覆盖。
    void abort(const std::string& id, const std::string& reason, std::int64_t now);
    // 在提交工位线程前记录 PENDING，命令通过 epoch/seq 唯一关联。
    void record_command(const std::string& inspection_id, const Command&);
    // 仅更新现场执行记录，不修改质量结果；now 为 Unix 毫秒。
    void acknowledge(const Command&, const std::string& status, const std::string& detail,
                     std::int64_t now);
    // 按检测编号或工件编号查询；空字符串查询全部历史，返回独立数据副本。
    Json query(const std::string& id_or_workpiece) const;
    // 返回内部业务事件，可用于导出；不转换为任何平台载荷。
    Json outbox() const;
    // 包含所有尚未得到平台接受确认的事件；本期始终为 PENDING。
    std::int64_t pending_count() const;
    // 实际执行短写事务，避免只检查权限却忽略数据库锁/介质写错误。
    void writable_probe();
    // 故障钩子只用于测试进程，生产默认为空；用来定位 COMMIT 前后崩溃边界。
    std::function<void(const std::string&)> checkpoint;

  private:
    // 模块三：私有连接和 SQL 执行工具。裸指针代表库句柄，生命周期由仓库管理。
    sqlite3* db_ = nullptr;
    void exec(const std::string&) const;
};
} // namespace inspection
