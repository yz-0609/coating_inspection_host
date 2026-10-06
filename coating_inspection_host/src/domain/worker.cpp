/**
 * 文件功能：检测业务状态机的完整实现
 *
 * 职责说明：串行处理事件，管理测量窗口、确认点、持久化、工位命令和故障。
 * 调用关系：run 消费队列 → handle 分发 → 对应业务函数；命令执行结果异步回到 command_done。
 * 阅读提示：主线是 begin → START 确认 → arm → 测量候选 → confirm → 保存 → 完成判定。
 */
// 检测状态机实现：只有本线程可改变活动任务、采集窗口和产品规则快照。
// 状态转换先落库再输出成功提示；设备通信始终通过异步命令接口。
#include "inspection/worker.hpp"
#include <filesystem>
#include <limits>
#include <unistd.h>
namespace inspection {
// 模块一：候选展示与按钮绑定。持同一把锁输出日志并更新编号，避免读到半更新状态。
void CandidateDisplay::publish(const Task& t, const Measurement& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    log("candidate", "收到新候选，请明确确认编号",
        {{"inspection_id", t.id},
         {"point_index", t.points.size() + 1},
         {"candidate_id", m.candidate_id},
         {"milli_um", m.thickness_milli_um},
         {"substrate", m.substrate}});
    current_ = m.candidate_id;
}
// 返回字符串副本，释放锁后工位线程仍能安全使用当时编号。
std::string CandidateDisplay::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}
// 清除按钮可确认编号；新窗口、完成、中止或故障后旧展示必须失效。
void CandidateDisplay::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.clear();
}
// 模块二：生命周期与状态查询。构造只保存依赖，数据库稍后由业务线程创建。
InspectionWorker::InspectionWorker(Config cfg, const IClock& clock, BoundedQueue<Event>& queue,
                                   BusinessHealth& health, CandidateDisplay& display,
                                   IInstrument& instrument, IWorkstation& workstation)
    : cfg_(std::move(cfg)), clock_(clock), queue_(queue), health_(health), display_(display),
      instrument_(instrument), workstation_(workstation) {
}
// 析构兜底请求停止并等待退出，避免对象销毁后后台仍使用 this。
InspectionWorker::~InspectionWorker() {
    request_stop();
    join();
}
// lambda 捕获 this，在新线程中执行 run；调用方应只启动一次。
void InspectionWorker::start() {
    thread_ = std::thread([this] { run(); });
}
// 原子标志允许 main 或外部控制线程安全请求退出；队列等待最多 20 ms。
void InspectionWorker::request_stop() {
    stop_requested_.store(true);
}
// joinable 判断是否有可等待线程，避免对未启动或已等待的线程再次 join。
void InspectionWorker::join() {
    if (thread_.joinable())
        thread_.join();
}
// 只有待 START 确认或检测中的任务算活动任务，完成/中止后不再接受点位。
bool InspectionWorker::active() const {
    return task_ &&
           (task_->state == TaskState::StartPending || task_->state == TaskState::Inspecting);
}
// 按数据库所在分区的可用空间检查；配置的 MB 在这里按 1024×1024 字节换算。
bool InspectionWorker::disk_ok() const {
    auto space = std::filesystem::space(std::filesystem::path(cfg_.database_path).parent_path());
    return space.available >= static_cast<std::uintmax_t>(cfg_.disk_min_mb) * 1024U * 1024U;
}
// 在线、版本匹配且未超过健康期限才算可用，避免拿很久以前的现场状态执行操作。
bool InspectionWorker::usable_snapshot() const {
    return snapshot_.online && snapshot_.version == protocol_version &&
           clock_.mono_ms() - snapshot_.mono_ms <= cfg_.health_ms;
}
// 模块三：业务线程主循环。仓库生命周期完全落在本线程内，其他线程只投递事件。
void InspectionWorker::run() {
    try {
        // 连接在业务线程内创建、恢复、使用和销毁，绝不跨线程混用。
        repo_ = std::make_unique<InspectionRepository>(cfg_.database_path, cfg_.database_busy_ms);
        // 服务重启把未完成任务中止、未确认命令转 UNKNOWN；绝不自动恢复历史结果输出。
        repo_->recover(clock_.wall_ms());
        repo_->writable_probe();
        if (!disk_ok())
            throw std::runtime_error("存储可用空间低于启动阈值");
        run_id_ = uuid();
        quality_ = time_quality();
        // 仅测试配置允许读取崩溃钩子；_exit 立即终止，模拟提交前后断电式退出。
        if (cfg_.allow_test_controls)
            repo_->checkpoint = [](const std::string& name) {
                const char* target = std::getenv("INSPECTION_TEST_CRASH_AT");
                if (target && name == target)
                    ::_exit(86);
            };
        log("service_ready", "检测服务已启动；云端尚未配置",
            {{"run_id", run_id_}, {"cloud_status", cloud_.status()}, {"time_quality", quality_}});
        // 每轮先处理溢出，再取一个事件，然后检查现场快照、测量期限和磁盘余量。
        while (!stop_requested_.load()) {
            // 漏掉事件可能破坏检测顺序，队列溢出必须停止新业务，不能继续假定所有事件都在。
            if (queue_.take_overflow())
                fatal("event_queue_overflow");
            Event event;
            if (queue_.pop(event, std::chrono::milliseconds(20))) {
                try {
                    handle(event);
                } catch (const std::exception& e) {
                    fatal(std::string("business_or_database_failure: ") + e.what());
                }
            }
            const auto now = clock_.mono_ms();
            // 即便没有新的离线事件，陈旧现场快照也会导致活动任务中止。
            if (active() && snapshot_.online && now - snapshot_.mono_ms > cfg_.health_ms)
                abort("workstation_snapshot_expired");
            // 采集总期限涵盖等待新测量和等待确认；新候选出现不会延长当前点的截止时间。
            if (active() &&
                (phase_ == InspectionPhase::WaitNewMeasurement ||
                 phase_ == InspectionPhase::WaitConfirm) &&
                now >= window_deadline_)
                abort("measurement_timeout");
            // 每秒检查空间，减少对文件系统的频繁查询。
            if (now - last_disk_check_ >= 1000) {
                last_disk_check_ = now;
                if (!disk_ok())
                    fatal("disk_space_below_threshold");
            }
            // 只有正常循环才能推进健康戳；通信线程不能代替业务线程证明业务健康。
            if (!fatal_)
                health_.last_loop_ms.store(clock_.mono_ms());
        }
        shutdown();
        repo_.reset();
    } catch (const std::exception& e) {
        health_.enabled.store(false);
        failed_.store(true);
        log("startup_or_shutdown_error", e.what());
        repo_.reset();
    }
    // 无论正常收尾还是异常，最终发布完成标志，让 main 知道可以停止设备线程。
    finished_.store(true);
}
// 模块四：事件分发。所有异步输入在这里串行执行，避免任务状态被多个线程同时修改。
void InspectionWorker::handle(const Event& e) {
    switch (e.kind) {
    case EventKind::Stop:
        stop_requested_.store(true);
        break;
    // 检测中断线或连接代次变化立即中止；新连接不能沿用旧检测窗口。
    case EventKind::InstrumentState:
        if (active() && (!e.online || (generation_ && e.generation != generation_)))
            abort("instrument_disconnected_or_generation_changed");
        instrument_online_ = e.online;
        generation_ = e.generation;
        log("instrument_state", e.online ? "仪器在线" : "仪器离线", {{"generation", e.generation}});
        break;
    // 测量过滤：必须正在检测、处于采集窗口、接收时间严格晚于 arm、代次一致且材料有效。
    case EventKind::Measurement: {
        const auto& m = e.measurement;
        if (fatal_ || !active() || task_->state != TaskState::Inspecting ||
            (phase_ != InspectionPhase::WaitNewMeasurement &&
             phase_ != InspectionPhase::WaitConfirm) ||
            m.mono_ms <= window_ms_ || m.generation != window_generation_ ||
            (m.substrate != "FE" && m.substrate != "NFE")) {
            log("measurement_ignored", "测量未归属于当前采集窗口",
                {{"candidate_id", m.candidate_id}, {"generation", m.generation}});
            break;
        }
        // 同一窗口最多保存 128 个候选；厚度相同仍可能是独立测量，不能按数值去重。
        if (candidates_.size() >= 128) {
            fatal("candidate_buffer_overflow");
            break;
        }
        // 保存候选副本后才展示；候选仅等待人工确认，不会自动写入确认点表。
        candidates_.emplace(m.candidate_id, m);
        phase_ = InspectionPhase::WaitConfirm;
        display_.publish(*task_, m);
        break;
    }
    case EventKind::Snapshot:
        snapshot(e);
        break;
    case EventKind::CommandDone:
        command_done(e);
        break;
    case EventKind::Cli:
        cli(e);
        break;
    }
}
// 模块五：工位快照。先保存前次状态，供掉线、复位和边沿变化判断。
void InspectionWorker::snapshot(const Event& e) {
    const Snapshot previous = snapshot_;
    snapshot_ = e.snapshot;
    // 在线转离线时取消同步资格，中止活动任务，并使当前按钮展示失效。
    if (!snapshot_.online) {
        if (link_online_) {
            synchronized_ = false;
            link_online_ = false;
            if (active())
                abort("workstation_offline");
            display_.clear();
        }
        return;
    }
    // 协议版本错配时停止业务与健康心跳，避免按错误寄存器含义操作现场。
    if (snapshot_.version != protocol_version) {
        synchronized_ = false;
        fatal("protocol_version_mismatch");
        log("protocol_error", "协议版本不匹配，停止业务命令与健康心跳");
        return;
    }
    // 无符号 uptime 差值允许正常 32 位回绕；向后超过半区间视为复位。
    bool reboot = link_online_ &&
                  (static_cast<std::uint32_t>(snapshot_.uptime - previous.uptime) > 0x80000000U ||
                   (snapshot_.state == McuState::Boot && previous.state != McuState::Boot) ||
                   (previous.ack_epoch == epoch_ && snapshot_.ack_epoch != epoch_));
    // 初次上线或 MCU 重启都需要新 epoch 的安全同步；旧 token 和旧 ACK 不可信。
    if (!link_online_ || reboot) {
        link_online_ = true;
        synchronized_ = false;
        if (active())
            abort("mcu_restart");
        if (!fatal_) {
            resync_requested_ = true;
            synchronize();
        }
        return;
    }
    // START 确认后的活动检测必须持续满足到位、token、检测状态和设备健康。
    if (active() && task_->state == TaskState::Inspecting) {
        if (!snapshot_.present() || snapshot_.token != task_->token ||
            snapshot_.state != McuState::Inspecting || snapshot_.health == 0) {
            abort("workstation_state_contradiction");
            if (snapshot_.state == McuState::Fault) {
                synchronized_ = false;
                resync_requested_ = true;
                synchronize();
            }
            return;
        }
    }
    // 外部故障（尤其业务停滞导致的心跳超时）恢复后仍需新 epoch + 人工复位。
    // HOST_ABORT 是我们主动中止后的预期故障，沿用该 epoch 等待人工复位即可。
    if (synchronized_ && snapshot_.state == McuState::Fault && previous.state != McuState::Fault &&
        snapshot_.fault != 5) {
        synchronized_ = false;
        resync_requested_ = true;
        phase_ = InspectionPhase::Fault;
        synchronize();
        return;
    }
    // 工件完成并正常离开，MCU 回到 Idle 后释放当前任务，保留数据库历史。
    if (task_ && task_->state == TaskState::Completed && snapshot_.state == McuState::Idle &&
        !snapshot_.present()) {
        task_.reset();
        phase_ = InspectionPhase::Idle;
        display_.clear();
    }
    // 锁存按钮可被多次轮询读到，用 event_sequence 去重；完成处理后再发 AckInput。
    if (snapshot_.event_code != 0 && (!event_seen_ || snapshot_.event_sequence != last_event_) &&
        synchronized_ && !fatal_) {
        last_event_ = snapshot_.event_sequence;
        event_seen_ = true;
        try {
            // ACTION 在 WaitArm 开窗，在 WaitConfirm 确认事件入队时绑定的候选；其他阶段拒绝。
            if (snapshot_.event_code == 1) {
                if (phase_ == InspectionPhase::WaitArm)
                    arm();
                else if (phase_ == InspectionPhase::WaitConfirm)
                    confirm(e.bound_candidate);
                else
                    log("button_rejected", "ACTION 在当前步骤无效");
            } else if (snapshot_.event_code == 2) {
                // ACK 按钮先入串行命令队列，再请求复位；RESET 不会静默取消正在检测的任务。
                issue(Opcode::AckInput, last_event_);
                if (!active() && snapshot_.state == McuState::Fault)
                    issue(Opcode::ResetFault);
                else
                    log("button_rejected", "当前状态不允许复位");
                return;
            } else
                log("button_rejected", "未知按钮事件类型");
        } catch (const std::exception& ex) {
            log("button_rejected", ex.what());
        }
        issue(Opcode::AckInput, last_event_);
    }
}
// 模块六：主站安全同步。旧命令全收尾后分配持久化新 epoch，并重置周期内序号。
void InspectionWorker::synchronize() {
    if (!commands_.empty())
        return;
    resync_requested_ = false;
    epoch_ = repo_->allocate("host_epoch");
    sequence_ = 0;
    last_event_ = 0;
    event_seen_ = false;
    phase_ = InspectionPhase::Fault;
    issue(Opcode::SyncSafe);
}
// 命令签发：生成唯一身份、先落库 PENDING，再送工位线程；成功入队不等于现场执行成功。
void InspectionWorker::issue(Opcode opcode, std::uint32_t argument) {
    // 禁止 32 位序号回绕复用；无活动任务时请求新同步周期，活动任务须中止。
    if (sequence_ == std::numeric_limits<std::uint32_t>::max()) {
        synchronized_ = false;
        if (active())
            throw std::runtime_error("命令序号耗尽，当前任务需中止");
        synchronize();
        throw std::runtime_error("已重新同步，人工复位后重试操作");
    }
    Command c;
    c.epoch = epoch_;
    c.sequence = ++sequence_;
    c.opcode = opcode;
    c.argument = argument;
    // 安全同步/人工复位不关联任务 token；其余命令使用当前检测 token。
    c.token = (opcode == Opcode::SyncSafe || opcode == Opcode::ResetFault)
                  ? 0
                  : (task_ ? task_->token : 0);
    // 先记录再提交：即使提交后进程突然退出，重启也能知道曾尝试过哪条命令。
    repo_->record_command(task_ ? task_->id : std::string{}, c);
    commands_.emplace(c.sequence, c);
    // 入队失败立即记录 FAILED 并移除待确认映射，抛异常交业务入口处理。
    if (!workstation_.submit(c)) {
        repo_->acknowledge(c, "FAILED", "command_queue_full", clock_.wall_ms());
        commands_.erase(c.sequence);
        throw std::runtime_error("工位命令队列已满");
    }
}
// 模块七：创建检测。工位就绪/到位、健康、仪器在线、已同步且无待确认命令才允许开始。
void InspectionWorker::begin(const std::string& workpiece, const std::string& product) {
    if (fatal_ || active() || !commands_.empty() || !synchronized_ || !usable_snapshot() ||
        snapshot_.state != McuState::Ready || !snapshot_.present() || snapshot_.health == 0 ||
        !instrument_online_)
        throw std::runtime_error("开始条件未满足：检查同步、到位、仪器、健康及活动任务");
    if (workpiece.empty() || workpiece.size() > 128 ||
        workpiece.find_first_of("\r\n\t") != std::string::npos)
        throw std::runtime_error("工件编号无效");
    auto rule = cfg_.rules.find(product);
    if (rule == cfg_.rules.end())
        throw std::runtime_error("未知产品型号");
    if (!disk_ok())
        throw std::runtime_error("磁盘空间不足");
    repo_->writable_probe();
    // 每次分配新的检测 UUID 和持久化 session_token，并复制产品规则作为不可变快照。
    Task task;
    task.id = uuid();
    task.workpiece = workpiece;
    task.rule = rule->second;
    task.token = repo_->allocate("session_token");
    task.started_ms = clock_.wall_ms();
    // 数据库提交成功后才设置内存当前任务和展示成功提示，随后发送 START 等待业务 ACK。
    repo_->create(task, quality_, run_id_);
    task_ = std::move(task);
    phase_ = InspectionPhase::StartPending;
    display_.clear();
    candidates_.clear();
    log("task_created", "任务及规则快照已保存，等待 START 确认",
        {{"inspection_id", task_->id}, {"session_token", task_->token}});
    issue(Opcode::Start);
}
// 模块八：独立点位采集窗口。只能在 WaitArm 开启，每个确认点都需重新 arm。
void InspectionWorker::arm() {
    if (fatal_ || !active() || task_->state != TaskState::Inspecting ||
        phase_ != InspectionPhase::WaitArm || !instrument_online_ || !usable_snapshot() ||
        !snapshot_.present())
        throw std::runtime_error("当前步骤不允许开启采集窗口");
    candidates_.clear();
    display_.clear();
    // 记录开启时刻和当前连接代次，之后只接纳该代次且接收时刻更晚的测量。
    window_ms_ = clock_.mono_ms();
    window_generation_ = generation_;
    window_deadline_ = window_ms_ + cfg_.measurement_ms;
    phase_ = InspectionPhase::WaitNewMeasurement;
    log("window_armed", "采集窗口已开启，请产生新的测量",
        {{"inspection_id", task_->id},
         {"point_index", task_->points.size() + 1},
         {"generation", generation_}});
}
// 模块九：人工确认与最终判定。明确指定候选 ID，不能把未知/旧窗口编号当成当前点。
void InspectionWorker::confirm(const std::string& id) {
    if (fatal_ || !active() || phase_ != InspectionPhase::WaitConfirm || !usable_snapshot() ||
        !snapshot_.present())
        throw std::runtime_error("当前步骤不允许确认点位");
    auto candidate = candidates_.find(id);
    if (candidate == candidates_.end())
        throw std::runtime_error("候选编号无效或不属于当前窗口");
    Point p;
    p.index = static_cast<int>(task_->points.size()) + 1;
    p.value = candidate->second;
    p.confirmed_ms = clock_.wall_ms();
    // 点位事务提交后才追加内存列表；若写入失败，不会显示已经保存。
    repo_->save_point(task_->id, p);
    task_->points.push_back(p);
    display_.clear();
    candidates_.clear();
    log("point_saved", "确认点已提交保存",
        {{"inspection_id", task_->id}, {"point_index", p.index}, {"candidate_id", id}});
    // 点数未足则返回 WaitArm；清空候选使同一窗口不能用于下一个点。
    if (task_->points.size() < static_cast<std::size_t>(task_->rule.required_points)) {
        phase_ = InspectionPhase::WaitArm;
        return;
    }
    // 先计算内存结果，再由仓库根据持久化规则/点位复核，并原子写结果和 Outbox。
    phase_ = InspectionPhase::Finalizing;
    task_->result = passes(task_->rule, task_->points) ? "PASS" : "FAIL";
    repo_->complete(*task_, cfg_.station_id, clock_.wall_ms());
    task_->state = TaskState::Completed;
    phase_ = InspectionPhase::Completed;
    log("inspection_completed", "质量结果与待传事件已原子提交，现场执行等待确认",
        {{"inspection_id", task_->id}, {"result", task_->result}});
    // 质量已提交后才请求 MCU 应用输出；现场动作失败也不能覆盖既有 PASS/FAIL。
    issue(Opcode::ApplyResult, task_->result == "PASS" ? 1U : 2U);
}
// 模块十：业务中止。只更新活动任务，保留确认点；现场在线且版本正确时请求撤销输出。
void InspectionWorker::abort(const std::string& reason) {
    if (!active())
        return;
    repo_->abort(task_->id, reason, clock_.wall_ms());
    task_->state = TaskState::Aborted;
    phase_ = InspectionPhase::Aborted;
    display_.clear();
    candidates_.clear();
    log("inspection_aborted", "检测已中止，已确认点保留",
        {{"inspection_id", task_->id}, {"reason", reason}});
    if (snapshot_.online && snapshot_.version == protocol_version && epoch_)
        issue(Opcode::Abort);
}
// 模块十一：异步命令结果。根据 epoch 丢弃旧周期回调，更新现场执行记录。
void InspectionWorker::command_done(const Event& e) {
    if (e.command.epoch != epoch_) {
        log("stale_command_event", "丢弃旧主站周期的命令回调");
        return;
    }
    commands_.erase(e.command.sequence);
    // 超时/传输未知表示 UNKNOWN：无法证明是否已执行；明确拒绝/不满足状态表示 FAILED。
    std::string outcome = e.success ? "ACKNOWLEDGED"
                                    : (e.detail.find("timeout") != std::string::npos ||
                                               e.detail.find("unknown") != std::string::npos
                                           ? "UNKNOWN"
                                           : "FAILED");
    repo_->acknowledge(e.command, outcome, e.detail, clock_.wall_ms());
    log("command_done", e.success ? "工位业务执行已确认" : "工位业务执行未确认",
        {{"opcode", opcode_name(e.command.opcode)},
         {"command_seq", e.command.sequence},
         {"status", outcome},
         {"detail", e.detail},
         {"ack_status", e.snapshot.ack_status},
         {"ack_host_epoch", e.snapshot.ack_epoch},
         {"ack_command_seq", e.snapshot.ack_sequence},
         {"mcu_state", static_cast<int>(e.snapshot.state)},
         {"session_token", e.snapshot.token},
         {"outputs", e.snapshot.outputs}});
    // 成功回调携带验证后的快照；不同命令决定同步、复位或进入检测阶段。
    if (e.success) {
        snapshot_ = e.snapshot;
        if (e.command.opcode == Opcode::SyncSafe) {
            synchronized_ = true;
            phase_ = InspectionPhase::Fault;
            log("sync_complete", "安全同步完成，请人工 reset");
        } else if (e.command.opcode == Opcode::ResetFault) {
            phase_ = snapshot_.present() ? InspectionPhase::Ready : InspectionPhase::Idle;
            if (task_ && task_->state == TaskState::Aborted)
                task_.reset();
        } else if (e.command.opcode == Opcode::Start && task_ &&
                   task_->state == TaskState::StartPending) {
            // 只有 MCU START 确认后才把任务持久化为 INSPECTING，随后允许 arm。
            repo_->mark_started(task_->id);
            task_->state = TaskState::Inspecting;
            phase_ = InspectionPhase::WaitArm;
            log("inspection_started", "START 已确认，可以开启点位窗口",
                {{"inspection_id", task_->id}});
        }
    } else {
        if (e.command.opcode == Opcode::Start)
            abort("start_rejected_or_timeout");
        // 应用结果失败时保留质量结果，进入现场故障并尝试撤销输出，阻止错误地开启新任务。
        if (e.command.opcode == Opcode::ApplyResult) {
            phase_ = InspectionPhase::Fault;
            log("actuation_fault", "质量结果保留，现场执行失败或未知，禁止新任务");
            if (snapshot_.online)
                issue(Opcode::Abort);
        }
        if (e.command.opcode == Opcode::SyncSafe)
            synchronized_ = false;
    }
    // 需要重新同步时，等待所有旧命令收尾并确保通道在线，避免两个 epoch 混用。
    if (resync_requested_ && !synchronized_ && snapshot_.online &&
        snapshot_.version == protocol_version && commands_.empty() && !fatal_)
        synchronize();
}
// 模块十二：状态展示。汇总业务、设备、候选点数和本地待传数量，便于操作者检查开始条件。
void InspectionWorker::status() {
    log("status", "当前检测状态",
        {{"phase", phase_name(phase_)},
         {"instrument_online", instrument_online_},
         {"generation", generation_},
         {"workstation_online", snapshot_.online},
         {"mcu_state", static_cast<int>(snapshot_.state)},
         {"outputs", snapshot_.outputs},
         {"present", snapshot_.present()},
         {"synchronized", synchronized_},
         {"fatal", fatal_},
         {"inspection_id", task_ ? task_->id : ""},
         {"confirmed_points", task_ ? task_->points.size() : 0},
         {"cloud_status", cloud_.status()},
         {"pending_events", repo_->pending_count()}});
}
// 模块十三：不可恢复故障。只处理首次故障，标记失败并停止健康心跳，要求重启处理。
void InspectionWorker::fatal(const std::string& reason) {
    if (fatal_)
        return;
    fatal_ = true;
    failed_.store(true);
    health_.enabled.store(false);
    phase_ = InspectionPhase::Fault;
    display_.clear();
    log("fatal", "系统故障，停止新任务与应用心跳", {{"reason", reason}});
    // 即使数据库已出错仍尝试保存中止；失败则记录原因，重启 recover 再处理未完成任务。
    try {
        abort(reason);
    } catch (const std::exception& e) {
        log("abort_persist_failed", "无法持久化中止记录，重启恢复将重新处理",
            {{"detail", e.what()}});
    }
}
// 模块十四：有界收尾。活动任务保存服务退出中止；完成任务也请求撤销现场输出。
void InspectionWorker::shutdown() {
    health_.enabled.store(false);
    try {
        if (active())
            abort("service_shutdown");
        else if (task_ && task_->state == TaskState::Completed && snapshot_.online)
            issue(Opcode::Abort);
    } catch (const std::exception& e) {
        log("shutdown_abort_error", e.what());
    }
    // 业务线程继续消费命令结果，但不推进健康心跳；等候有上限，强制退出由 MCU 超时兜底。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.command_ms);
    // 只消费命令结果到截止时间；不再接受测量确认或新任务，避免退出过程中修改质量结果。
    while (!commands_.empty() && std::chrono::steady_clock::now() < deadline) {
        Event e;
        if (queue_.pop(e, std::chrono::milliseconds(20)) && e.kind == EventKind::CommandDone) {
            try {
                repo_->acknowledge(e.command, e.success ? "ACKNOWLEDGED" : "UNKNOWN", e.detail,
                                   clock_.wall_ms());
                commands_.erase(e.command.sequence);
            } catch (const std::exception& ex) {
                log("shutdown_log_error", ex.what());
                break;
            }
        }
    }
    log("service_stopped", "检测业务已停止");
}
} // namespace inspection
