/**
 * 文件功能：SQLite 仓库、事务与查询实现
 *
 * 职责说明：用预编译语句保存业务记录，确保最终结果与 Outbox 原子提交。
 * 调用关系：InspectionWorker 调用仓库 → Statement 绑定/执行 SQL → Transaction 提交或回滚。
 * 阅读提示：参数用 ? 占位并绑定；SQL 参数从 1 开始，而结果列从 0 开始。
 */
// SQLite 实现：Statement/Transaction 管理资源和自动回滚，StorageError 向业务层传播。
// 四类业务表分别描述检测、确认点、待传事件和现场命令，不混淆质量与执行状态。
#include "inspection/repository.hpp"
#include "inspection/schema.hpp"
#include <filesystem>
#include <limits>
#include <stdexcept>
namespace inspection {
namespace {
// 模块一：SQL 语句资源封装（RAII）。构造 prepare，析构 finalize，异常退出也释放资源。
class Statement {
  public:
    // 预编译 SQL，得到可绑定和执行的 sqlite3_stmt；prepare 失败转换为 StorageError。
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &value_, nullptr) != SQLITE_OK)
            throw StorageError(sqlite3_errmsg(db));
    }
    // 释放语句对象，避免占用数据库锁或泄漏 SQLite 资源。
    ~Statement() {
        sqlite3_finalize(value_);
    }
    // 绑定字符串参数；SQLITE_TRANSIENT 要求 SQLite 复制内容，不依赖调用者字符串寿命。
    void text(int index, const std::string& value) {
        check(sqlite3_bind_text(value_, index, value.data(), static_cast<int>(value.size()),
                                SQLITE_TRANSIENT));
    }
    // 绑定 64 位整数参数，用于时间、厚度和身份，避免文本拼接或整数精度损失。
    void integer(int index, std::int64_t value) {
        check(sqlite3_bind_int64(value_, index, value));
    }
    // step 推进语句：ROW 有一条结果，DONE 已结束；锁、约束、介质错误都抛异常。
    bool row() {
        const int rc = sqlite3_step(value_);
        if (rc == SQLITE_ROW)
            return true;
        if (rc == SQLITE_DONE)
            return false;
        throw StorageError(sqlite3_errmsg(db_));
    }
    // 执行预期无查询结果的 INSERT/UPDATE；若意外得到行，说明调用方式或 SQL 不匹配。
    void done() {
        if (row())
            throw StorageError("语句意外返回记录");
    }
    // 读取当前结果行的整数列，列索引从 0 开始，与绑定参数从 1 开始不同。
    std::int64_t integer(int index) const {
        return sqlite3_column_int64(value_, index);
    }
    // 复制 SQLite 临时文本；NULL 变空字符串，调用者不会持有结果缓冲指针。
    std::string text(int index) const {
        auto p = sqlite3_column_text(value_, index);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string{};
    }

  private:
    // 统一检查 bind 返回值，确保绑定失败不能继续用缺参数的语句写数据。
    void check(int rc) {
        if (rc != SQLITE_OK)
            throw StorageError(sqlite3_errmsg(db_));
    }
    sqlite3* db_;
    sqlite3_stmt* value_ = nullptr;
};
// 模块二：事务封装。BEGIN IMMEDIATE 先获得写事务；未 commit 的对象析构时自动回滚。
class Transaction {
  public:
    // 提前申请写锁，减少“读出旧状态后才发现不能写”的情况；锁等待由 busy_timeout 限制。
    explicit Transaction(sqlite3* db) : db_(db) {
        if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw StorageError(sqlite3_errmsg(db_));
    }
    // 异常或提前 return 时 committed_ 仍为 false，ROLLBACK 撤销本事务全部修改。
    ~Transaction() {
        if (!committed_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    // 只有 COMMIT 成功才设 committed_；提交失败仍保留自动回滚行为。
    void commit() {
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw StorageError(sqlite3_errmsg(db_));
        committed_ = true;
    }

  private:
    sqlite3* db_;
    bool committed_ = false;
};
// 模块三：规则快照序列化。保存数值、单位、材料和版本，便于以后重建当次判定依据。
Json rule_json(const Rule& r) {
    return {{"product_id", r.product_id},
            {"required_points", r.required_points},
            {"min_milli_um", r.min_milli_um},
            {"max_milli_um", r.max_milli_um},
            {"substrate", r.substrate},
            {"rule_version", r.version},
            {"unit", "milli_um"}};
}
} // namespace
// 模块四：打开数据库与首次建库。目录不存在时自动创建，只读模式不会创建文件。
InspectionRepository::InspectionRepository(const std::string& path, int busy_ms, bool readonly) {
    if (!readonly && path != ":memory:" && !std::filesystem::path(path).parent_path().empty())
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    // 按用途选择只读/可写；NOMUTEX 依赖单线程独占连接，不能让多个线程同时使用此仓库。
    const int flags =
        readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (sqlite3_open_v2(path.c_str(), &db_, flags | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "数据库打开失败";
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        throw StorageError(error);
    }
    try {
        // 锁最多等待 busy_ms，外键约束强制开启，避免测量/命令记录指向不存在的任务。
        sqlite3_busy_timeout(db_, busy_ms);
        exec("PRAGMA foreign_keys=ON");
        std::int64_t v = 0;
        {
            Statement version(db_, "PRAGMA user_version");
            version.row();
            v = version.integer(0);
        }
        // user_version 是结构版本；遇到更新版数据库拒绝继续，防止旧程序误写。
        if (v > 1)
            throw StorageError("数据库版本高于程序支持版本，禁止降级写入");
        if (readonly)
            return;
        // WAL 使用预写日志，FULL 要求更强的提交同步；最终耐久性仍依赖系统和存储硬件。
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=FULL");
        // 首次建库用一个事务执行嵌入的迁移 SQL，失败则不会留下半套表结构。
        if (v == 0) {
            Transaction transaction(db_);
            exec(schema_v1);
            transaction.commit();
        }
    // 构造失败也释放已打开的句柄，因为未成功构造的对象不会执行自身析构函数。
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}
// 生命周期收尾：关闭本线程拥有的连接；所有临时 Statement 应已离开作用域。
InspectionRepository::~InspectionRepository() {
    if (db_)
        sqlite3_close(db_);
}
// 执行无结果/无需绑定参数的固定 SQL；业务输入使用 Statement 绑定。
void InspectionRepository::exec(const std::string& sql) const {
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        throw StorageError(sqlite3_errmsg(db_));
}
// 模块五：重启恢复。未完成任务中止，待确认现场动作变 UNKNOWN，待回复云事件回到 PENDING。
void InspectionRepository::recover(std::int64_t now) {
    Transaction tx(db_);
    Statement aborts(
        db_,
        "UPDATE inspection SET state='ABORTED',abort_reason='service_restart',finished_at_ms=? "
        "WHERE state IN('START_PENDING','INSPECTING')");
    aborts.integer(1, now);
    aborts.done();
    // 重启丢失了执行确认上下文，不能宣称 PENDING 命令执行失败或成功。
    exec("UPDATE actuation_log SET status='UNKNOWN',detail='service_restart' WHERE "
         "status='PENDING'");
    exec("UPDATE outbox SET state='PENDING',request_id=NULL WHERE state "
         "IN('WAIT_REPLY','RETRY_WAIT')");
    tx.commit();
}
// 模块六：持久化身份分配。事务内读取、递增、保存，服务重启后继续递增。
std::uint32_t InspectionRepository::allocate(const std::string& kind) {
    if (kind != "host_epoch" && kind != "session_token")
        throw StorageError("不支持的标识分配类型");
    Transaction tx(db_);
    // INSERT OR IGNORE 仅为首次使用建立计数；后续不会把旧值重置为零。
    Statement init(db_, "INSERT OR IGNORE INTO metadata VALUES(?,0)");
    init.text(1, kind);
    init.done();
    Statement read(db_, "SELECT value FROM metadata WHERE key=?");
    read.text(1, kind);
    read.row();
    auto value = read.integer(0);
    // 达到最大值后拒绝分配，禁止回绕为零或复用已发出的 token/epoch。
    if (value >= std::numeric_limits<std::uint32_t>::max())
        throw StorageError("32位标识空间耗尽，禁止重复使用");
    Statement write(db_, "UPDATE metadata SET value=? WHERE key=?");
    write.integer(1, value + 1);
    write.text(2, kind);
    write.done();
    tx.commit();
    return static_cast<std::uint32_t>(value + 1);
}
// 模块七：任务创建。写入工件身份、规则快照和时间质量；初始状态为 START_PENDING。
void InspectionRepository::create(const Task& t, const std::string& quality,
                                  const std::string& run) {
    Transaction tx(db_);
    Statement s(db_, "INSERT INTO "
                     "inspection(inspection_id,workpiece_id,product_id,session_token,state,"
                     "required_points,rule_version,rule_snapshot,started_at_ms,time_quality,run_id)"
                     " VALUES(?,?,?,?,'START_PENDING',?,?,?,?,?,?)");
    s.text(1, t.id);
    s.text(2, t.workpiece);
    s.text(3, t.rule.product_id);
    s.integer(4, t.token);
    s.integer(5, t.rule.required_points);
    s.text(6, t.rule.version);
    s.text(7, rule_json(t.rule).dump());
    s.integer(8, t.started_ms);
    s.text(9, quality);
    s.text(10, run);
    s.done();
    tx.commit();
}
// 单条 UPDATE 在 SQLite 自动提交事务内完成，只允许 START_PENDING → INSPECTING。
void InspectionRepository::mark_started(const std::string& id) {
    Statement s(
        db_,
        "UPDATE inspection SET state='INSPECTING' WHERE inspection_id=? AND state='START_PENDING'");
    s.text(1, id);
    s.done();
    // 必须恰好更新一条，否则说明任务不存在或状态不合法，不能显示开始成功。
    if (sqlite3_changes(db_) != 1)
        throw StorageError("任务不能进入检测态");
}
// 模块八：保存确认点。状态检查和插入同事务，保证点数按顺序推进。
void InspectionRepository::save_point(const std::string& id, const Point& p) {
    Transaction tx(db_);
    Statement check(
        db_, "SELECT required_points,(SELECT count(*) FROM measurement WHERE inspection_id=?) FROM "
             "inspection WHERE inspection_id=? AND state='INSPECTING'");
    check.text(1, id);
    check.text(2, id);
    // 点位索引必须等于已存点数+1 且不超过规则点数，禁止跳号、覆盖或多存。
    if (!check.row() || p.index != check.integer(1) + 1 || p.index > check.integer(0))
        throw StorageError("点位必须按顺序确认且任务正在检测");
    // 依照迁移表列顺序保存候选、整数厚度、材料、仪器、代次和接收/确认时间。
    Statement s(db_, "INSERT INTO measurement VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    s.text(1, id);
    s.integer(2, p.index);
    s.text(3, p.value.candidate_id);
    s.integer(4, p.value.thickness_milli_um);
    s.text(5, p.value.substrate);
    s.text(6, p.value.instrument_id);
    s.text(7, p.value.group);
    s.integer(8, static_cast<std::int64_t>(p.value.generation));
    s.integer(9, static_cast<std::int64_t>(p.value.local_sequence));
    s.integer(10, p.value.wall_ms);
    s.integer(11, p.value.mono_ms);
    s.integer(12, p.confirmed_ms);
    s.done();
    tx.commit();
}
// 模块九：最终结果原子提交。重新读取真实已保存数据，质量结果和云事件一同提交。
void InspectionRepository::complete(const Task& t, const std::string& station, std::int64_t now) {
    Transaction tx(db_);
    // 从数据库读取确认点和创建时规则，最终判定不信任调用方的缓存/历史配置。
    Statement inspection(db_, "SELECT rule_snapshot,started_at_ms,time_quality FROM inspection "
                              "WHERE inspection_id=? AND state='INSPECTING'");
    inspection.text(1, t.id);
    if (!inspection.row())
        throw StorageError("任务状态不允许完成");
    Json rule = Json::parse(inspection.text(0)), measurements = Json::array();
    bool pass = true;
    Statement points(db_, "SELECT "
                          "point_index,candidate_id,thickness_milli_um,substrate_mode,received_at_"
                          "ms,confirmed_at_ms,instrument_id,connection_generation FROM measurement "
                          "WHERE inspection_id=? ORDER BY point_index");
    points.text(1, t.id);
    int expected = 1;
    // 按 point_index 顺序检查连续性，并依据创建时规则逐点检查阈值与材料。
    while (points.row()) {
        if (points.integer(0) != expected++)
            throw StorageError("确认点不连续");
        const auto value = points.integer(2);
        const auto material = points.text(3);
        // 任意点不满足即整体 FAIL；阈值使用 >= 和 <=，边界包含在合格区间内。
        pass = pass && value >= rule.at("min_milli_um").get<std::int64_t>() &&
               value <= rule.at("max_milli_um").get<std::int64_t>() &&
               material == rule.at("substrate").get<std::string>();
        measurements.push_back({{"point_index", points.integer(0)},
                                {"candidate_id", points.text(1)},
                                {"thickness_milli_um", value},
                                {"substrate", material},
                                {"received_at_ms", points.integer(4)},
                                {"confirmed_at_ms", points.integer(5)},
                                {"instrument_id", points.text(6)},
                                {"connection_generation", points.integer(7)}});
    }
    // 要求恰好达到规则点数；还要核对内存结果，防止调用方缓存与持久化事实不一致。
    if (measurements.size() != rule.at("required_points").get<std::size_t>())
        throw StorageError("点数不足，不能完成判定");
    const std::string result = pass ? "PASS" : "FAIL";
    if (t.result != result)
        throw StorageError("内存与持久化判定不一致");
    // 将任务标为 COMPLETED 并写最终结果，但此时尚未 COMMIT，后续失败仍会回滚。
    Statement update(
        db_,
        "UPDATE inspection SET state='COMPLETED',result=?,finished_at_ms=? WHERE inspection_id=?");
    update.text(1, result);
    update.integer(2, now);
    update.text(3, t.id);
    update.done();
    // Outbox 为这次完成事件分配稳定 UUID，载荷保存规则、测量和时间质量，便于后续重试追溯。
    const auto event = uuid();
    Json payload = {{"schema_version", 1},
                    {"event_id", event},
                    {"inspection_id", t.id},
                    {"station_id", station},
                    {"workpiece_id", t.workpiece},
                    {"product_id", t.rule.product_id},
                    {"rule_version", rule.at("rule_version")},
                    {"rule_snapshot", rule},
                    {"result", result},
                    {"required_points", measurements.size()},
                    {"measurements", measurements},
                    {"started_at_ms", inspection.integer(1)},
                    {"finished_at_ms", now},
                    {"time_quality", inspection.text(2)}};
    // 与结果在同一个事务插入 Outbox；避免“结果已完成，却没有可上传事件”的断层。
    Statement out(db_,
                  "INSERT INTO outbox(event_id,inspection_id,event_type,payload,created_at_ms) "
                  "VALUES(?,?,'InspectionCompleted',?,?)");
    out.text(1, event);
    out.text(2, t.id);
    out.text(3, payload.dump());
    out.integer(4, now);
    out.done();
    // 测试钩子可在提交前抛异常/退出验证回滚，在提交后退出验证结果与事件都已保留。
    if (checkpoint)
        checkpoint("before_final_commit");
    tx.commit();
    if (checkpoint)
        checkpoint("after_final_commit");
}
// 模块十：中止任务。WHERE 限制只更改未完成状态；不删除已确认点，也不覆盖最终 PASS/FAIL。
void InspectionRepository::abort(const std::string& id, const std::string& reason,
                                 std::int64_t now) {
    Statement s(db_, "UPDATE inspection SET state='ABORTED',abort_reason=?,finished_at_ms=? WHERE "
                     "inspection_id=? AND state IN('START_PENDING','INSPECTING')");
    s.text(1, reason);
    s.integer(2, now);
    s.text(3, id);
    s.done();
}
// 模块十一：现场动作日志。空检测编号转 SQL NULL，使同步等全局命令也可记录。
void InspectionRepository::record_command(const std::string& id, const Command& c) {
    Statement s(db_, "INSERT INTO "
                     "actuation_log(inspection_id,host_epoch,command_seq,command,command_payload,"
                     "status) VALUES(NULLIF(?,''),?,?,?,?,'PENDING')");
    s.text(1, id);
    s.integer(2, c.epoch);
    s.integer(3, c.sequence);
    s.text(4, opcode_name(c.opcode));
    s.text(5, Json{{"token", c.token}, {"argument", c.argument}}.dump());
    s.done();
}
// 按 epoch+sequence 更新单条命令结果；质量表与动作日志独立，执行失败不改质量结论。
void InspectionRepository::acknowledge(const Command& c, const std::string& status,
                                       const std::string& detail, std::int64_t now) {
    Statement s(db_, "UPDATE actuation_log SET status=?,detail=?,acknowledged_at_ms=? WHERE "
                     "host_epoch=? AND command_seq=?");
    s.text(1, status);
    s.text(2, detail);
    s.integer(3, now);
    s.integer(4, c.epoch);
    s.integer(5, c.sequence);
    s.done();
}
// 模块十二：历史查询。空 key 查询全部，其他 key 同时匹配检测编号或工件编号。
Json InspectionRepository::query(const std::string& key) const {
    Json result = Json::array();
    Statement s(
        db_, "SELECT "
             "inspection_id,workpiece_id,product_id,state,result,abort_reason,rule_snapshot,"
             "session_token,time_quality,started_at_ms,finished_at_ms,run_id FROM inspection WHERE "
             "?='' OR inspection_id=? OR workpiece_id=? ORDER BY started_at_ms,inspection_id");
    s.text(1, key);
    s.text(2, key);
    s.text(3, key);
    // 每条检测组成 JSON 对象，再关联其确认点与动作记录；返回纯数据副本。
    while (s.row()) {
        Json t = {{"inspection_id", s.text(0)},
                  {"workpiece_id", s.text(1)},
                  {"product_id", s.text(2)},
                  {"state", s.text(3)},
                  {"result", s.text(4)},
                  {"abort_reason", s.text(5)},
                  {"rule", Json::parse(s.text(6))},
                  {"session_token", s.integer(7)},
                  {"time_quality", s.text(8)},
                  {"started_at_ms", s.integer(9)},
                  {"finished_at_ms", s.integer(10)},
                  {"run_id", s.text(11)},
                  {"measurements", Json::array()},
                  {"actuation", Json::array()}};
        // 按点位顺序返回测量追溯字段，避免依赖插入或底层存储顺序。
        Statement p(db_,
                    "SELECT "
                    "point_index,candidate_id,thickness_milli_um,substrate_mode,received_at_ms,"
                    "confirmed_at_ms,connection_generation,instrument_id,group_name,local_sequence,"
                    "received_mono_ms FROM measurement WHERE inspection_id=? ORDER BY point_index");
        p.text(1, s.text(0));
        while (p.row())
            t["measurements"].push_back({{"point_index", p.integer(0)},
                                         {"candidate_id", p.text(1)},
                                         {"thickness_milli_um", p.integer(2)},
                                         {"substrate", p.text(3)},
                                         {"received_at_ms", p.integer(4)},
                                         {"confirmed_at_ms", p.integer(5)},
                                         {"connection_generation", p.integer(6)},
                                         {"instrument_id", p.text(7)},
                                         {"group", p.text(8)},
                                         {"local_sequence", p.integer(9)},
                                         {"received_mono_ms", p.integer(10)}});
        // 返回现场命令执行历史，让使用者区分质量 PASS/FAIL 和 MCU 输出是否确认。
        Statement a(db_, "SELECT host_epoch,command_seq,command,status,detail,acknowledged_at_ms "
                         "FROM actuation_log WHERE inspection_id=? ORDER BY id");
        a.text(1, s.text(0));
        while (a.row())
            t["actuation"].push_back({{"host_epoch", a.integer(0)},
                                      {"command_seq", a.integer(1)},
                                      {"command", a.text(2)},
                                      {"status", a.text(3)},
                                      {"detail", a.text(4)},
                                      {"acknowledged_at_ms", a.integer(5)}});
        result.push_back(std::move(t));
    }
    return result;
}
// 模块十三：导出业务事件及传输状态，不转换成平台专用载荷。
Json InspectionRepository::outbox() const {
    Json result = Json::array();
    Statement s(
        db_,
        "SELECT event_id,state,attempt_count,payload FROM outbox ORDER BY created_at_ms,event_id");
    while (s.row())
        result.push_back({{"event_id", s.text(0)},
                          {"state", s.text(1)},
                          {"attempt_count", s.integer(2)},
                          {"payload", Json::parse(s.text(3))}});
    return result;
}
// 统计尚未得到平台接受确认的所有状态；未接入云时通常保持 PENDING。
std::int64_t InspectionRepository::pending_count() const {
    Statement s(db_, "SELECT count(*) FROM outbox WHERE state!='PLATFORM_ACCEPTED'");
    s.row();
    return s.integer(0);
}
// 模块十四：真实写探针。更新 metadata 并提交，实际检验锁、权限和介质写入能力。
void InspectionRepository::writable_probe() {
    Transaction tx(db_);
    exec("INSERT INTO metadata VALUES('writable_probe',0) ON CONFLICT(key) DO UPDATE SET "
         "value=value+1");
    tx.commit();
}
} // namespace inspection
