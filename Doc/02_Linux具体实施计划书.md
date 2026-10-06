# 工业涂层质检与追溯系统：Linux 具体实施计划书

版本：V2.0  
日期：2026-10-02  
当前平台：Ubuntu PC，C++17；ARM Linux 为后续移植目标。  
范围：接入测厚仪、建立检测任务、多点检测与判定、MCU 通信、记录保存与云端上传。

共同约定见[整体计划第 9 节](./01_整体项目计划说明书.md#9-通信协议三份文档的共同约定)。MCU 对应任务见[MCU 实施计划](./03_MCU具体实施计划书.md)。寄存器、命令码和状态值以整体计划为共同协议来源，修改时同步两侧代码与测试。

## 1. 完成条件

在 Ubuntu 上使用真实 BLE 测厚仪和真实 STM32F4 工位完成一次有工件编号的多点检测。每个确认点和规则可查询；完成后输出结果，发送 MCU 命令并记录确认；通过 MQTT/TLS 上报 OneNET，断网时保留待传事件，恢复后收到平台回复。

系统重启不恢复旧任务的 PASS 输出。尚未完成的检测有明确中止记录。ARM 兼容设计不等于 ARM 已验证。

## 2. 工作包与实现路径

| 工作包 | 建议实现 | 最小验证 |
|---|---|---|
| L1 接入测厚仪 | 包装现有 BluezReader/FrameParser，统一事件回调 | 实机测量与原始通知对应 |
| L2 建立检测任务 | CLI 输入工件与型号；规则配置；任务 UUID 和短 token | 输入校验、复检、新任务编号 |
| L3 多点检测与判定 | 业务线程单独拥有状态；采集窗口与人工确认 | 通知不被自动累计成多个点 |
| L4 与 MCU 通信 | 成熟 Modbus 主站库＋WorkstationClient | 真实串口轮询、完整命令与业务 ACK |
| L5 保存和上传记录 | SQLite 短事务＋Outbox＋OneNET 适配器 | 崩溃恢复与网络恢复补传 |

先用 MockWorkstation 验证业务，再替换为实际 Modbus 通道。仪器和数据库从早期就使用真实实现，避免最终才发现数据含义不一致。

## 3. Linux 代码组织

~~~text
coating_inspection_host/
├── app/main.cpp
├── include/inspection/
│   ├── instrument.hpp
│   ├── domain.hpp
│   ├── workstation.hpp
│   ├── repository.hpp
│   ├── cloud.hpp
│   └── events.hpp
├── src/
│   ├── instrument/          # BLE 适配、连接与通知
│   ├── domain/              # 任务、点位、规则、状态机
│   ├── workstation/         # Modbus、快照、命令与心跳
│   ├── persistence/         # SQLite、迁移、Outbox
│   ├── cloud/               # MQTT、OneNET 编码与回复
│   ├── cli/                 # 工件录入、采集与确认操作
│   └── support/             # 队列、配置、日志与时钟
├── tests/
├── deploy/
└── CMakeLists.txt
~~~

业务对象使用普通 C++ 数据结构，不持有 GDBus、串口或 MQTT 对象。现有仪器库通过适配器调用，不复制协议解析代码。

### 线程与状态所有权

| 执行上下文 | 职责 | 禁止事项 |
|---|---|---|
| BlueZ/GLib 循环 | 连接、通知、解析后复制事件 | 不执行数据库和网络上传 |
| InspectionWorker | 状态机、点位确认、规则、数据库事务 | 不直接等待 Modbus 超时或 MQTT |
| WorkstationWorker | 独占串口、轮询、命令确认、应用心跳 | 不自行修改任务状态 |
| UploadWorker | 待传读取、编码、发送、回复和重试 | 不影响现场判定 |

CLI 可使用输入线程，但输入事件仍交给 InspectionWorker。SQLite 连接不跨线程混用；各线程使用独立连接、短事务和有界锁等待。业务心跳由 InspectionWorker 的健康循环推进，通信线程只能在该健康状态仍新鲜时继续向 MCU 更新计数。

事件队列有容量、满载、关闭与唤醒语义。初始容量可设 128，后续按实测修订。队列满时通过独立故障标志/通知触发中止，不能依赖再向已满队列投递一条故障事件。

## 4. L1：接入测厚仪

### 实施步骤

1. 在当前 Ubuntu 构建并运行现有 BLE 项目，记录 BlueZ、编译器、仪器配置和版本。
2. 录制单次测量、连续测量、相同数值测量、断电重连的原始通知与回调。
3. 包装为 BleThicknessInstrument，提供启动、停止、在线状态及测量/连接状态事件。
4. 每次完成新的连接订阅递增 connection_generation；新连接清理前一连接的未完成协议帧。
5. 回调复制数值和必要上下文后入队，保留 CRC/格式错误计数。

测量事件至少包含仪器标识、厚度原始定点值或明确换算值、FE/NFE 模式、组名、本机接收的 wall clock、monotonic clock、连接代次和本地序号。连接代次用于隔离重连前后数据，本地序号只用于诊断。

协议的大小、缩放和符号解释沿用已验证代码；厚度比较优先使用明确单位的整数，例如千分之一微米，避免边界比较含义不清。显示时转换为 μm，单位同时写入规则与测试。

### 验收

- 实机日志能解释每个有效值来自哪些通知。
- 错误 CRC 不进入合法候选测量，分片和连续帧解析正常。
- 断连和重连可见，旧半帧不会与新连接数据拼接。
- 不假定仪器通知具有唯一序号或一条通知对应一个物理点。

## 5. L2：建立检测任务

首版用 CLI 录入 workpiece_id 和 product_id，支持 status、start、arm、confirm、cancel、reset、query 操作。键盘输入型条码枪可直接作为输入源；工件编号不能由“工件到位”信号代替。

产品规则包括 required_points、min_um、max_um、allowed_substrate_mode、rule_version。点数限制为 1～5；上下限必须合法，点数大于零，基材模式为仪器已验证返回值。阈值由实际演示样品与需求选定，示例配置不能被解释为通用涂层标准。

创建任务前检查：无活动任务、工件已到位、仪器在线、MCU 同步且健康、身份与规则有效、数据库可写。

创建任务时产生 inspection_id，分配非零 session_token，保存工件、产品、规则完整快照与开始时间；随后向 MCU 发 START。收到业务确认后再进入 INSPECTING。START 被拒绝或超时则保存中止原因，不能默认为已开始。

同一工件允许复检，inspection_id 必须不同。活动任务内修改型号或规则必须先取消当前任务。

## 6. L3：多点检测与质量判定

### 点位操作流程

~~~text
WAIT_ARM
   │ CLI arm 或 ACTION
   ▼
WAIT_NEW_MEASUREMENT
   │ 当前窗口内收到新候选值
   ▼
WAIT_CONFIRM
   │ 展示点位/数值，操作员确认
   ▼
PERSIST_POINT
   │ SQLite COMMIT 成功
   ├── 尚未完成 → 下一点 WAIT_ARM
   └── 全部完成 → FINALIZING
~~~

在 WAIT_CONFIRM 收到更多报文时显示新增候选/诊断，不自动增加 point_index。候选值发生更新时，确认操作绑定终端展示的候选编号；不能在确认过程中悄悄替换为另一条测量。

接受条件：活动任务在 INSPECTING；采集窗口已开启；接收单调时间晚于开启时间；connection_generation 匹配；模式和数值格式有效。操作员确认明确绑定 candidate_id。人工流程能建立点位归属，但不能证明探头实际位于指定空间位置；成果表述应说明这一边界。

窗口之外数据只做诊断。BLE 断连、到位信号消失、MCU 状态矛盾、队列溢出或测量超时均中止任务。重连后不继续旧窗口。

质量判定：每个已确认点的厚度均满足 min ≤ value ≤ max，且基材模式满足规则时为 PASS；否则为 FAIL。系统故障或点数不足为 ABORTED，不混入质量 FAIL。

确认点之后不提供静默覆盖；需要重测时，首版中止并创建新检测。以后再加入显式作废点、替代点和审计记录。

## 7. L4：Modbus 主站与工位协同

Linux 使用成熟主站库实现 RTU，WorkstationWorker 独占串口。现有 linux_communication 的配置与错误处理经验可以复用，但不要同时用两个库打开并读写同一串口。

设备通过配置路径打开，优先使用 /dev/serial/by-id 或自定义 udev 稳定链接。波特率、校验、从站地址与总体协议一致；验证运行用户串口权限。

### 轮询与按钮事件

每 100 ms 读取整体计划中的输入寄存器快照。检查 PROTOCOL_VERSION；未知版本阻止开始任务。

INPUT_EVENT_SEQ 与最后处理编号不同且有待确认事件时，交给业务线程。业务接受或明确拒绝后发送 ACK_INPUT_EVENT。短按事件不依赖轮询恰好看到 GPIO 高电平。

### 命令与确认

- 通过单个 0x10 请求写完整 9 寄存器命令块。
- 同一时间仅一个待业务确认命令；心跳更新可以穿插。
- 写成功后轮询 ACK_HOST_EPOCH、ACK_CMD_SEQ、ACK_STATUS、状态和 token，确认来自当前主站周期。
- 有限重试使用同一 epoch、seq、token 和参数，不能每次生成新的开始命令。
- 数据库已提交结果但 MCU 未确认时，保留质量判定，现场执行记为 UNKNOWN/FAILED，并提示故障。

初次启动、MCU 复位或失联后重新连接执行 SYNC_SAFE，使旧输出与 token 失效；通过人工 RESET 后再准备新任务。不得在重启后从历史 PASS 记录自动重放放行命令。

### 应用心跳

计划 200 ms 更新 HOST_HEARTBEAT；仅当业务线程健康时间仍在阈值内且服务未故障退出时发送变化值。不能仅证明串口线程仍在运行。

读取到 MCU 状态退回 BOOT/FAULT、活动 token 消失或 uptime 倒退时，中止活动任务并重新同步。uptime 回绕按协议处理，不能把回绕误认作长期离线。

## 8. L5：SQLite 记录保存

每个连接设置 foreign_keys=ON；计划采用 WAL、synchronous=FULL 和有界 busy_timeout。验证目标文件系统/存储的实际断电行为；设置本身不能证明任意硬件故障下都不丢数据。

### 建议数据表

~~~sql
CREATE TABLE inspection (
    inspection_id TEXT PRIMARY KEY,
    workpiece_id TEXT NOT NULL,
    product_id TEXT NOT NULL,
    session_token INTEGER NOT NULL,
    state TEXT NOT NULL,
    result TEXT CHECK(result IN ('PASS','FAIL') OR result IS NULL),
    required_points INTEGER NOT NULL CHECK(required_points BETWEEN 1 AND 5),
    rule_version TEXT NOT NULL,
    rule_snapshot TEXT NOT NULL,
    started_at_ms INTEGER NOT NULL,
    finished_at_ms INTEGER,
    abort_reason TEXT,
    time_quality TEXT NOT NULL
);

CREATE TABLE measurement (
    inspection_id TEXT NOT NULL REFERENCES inspection(inspection_id),
    point_index INTEGER NOT NULL CHECK(point_index BETWEEN 1 AND 5),
    candidate_id TEXT NOT NULL,
    thickness_milli_um INTEGER NOT NULL,
    substrate_mode TEXT NOT NULL,
    instrument_id TEXT NOT NULL,
    connection_generation INTEGER NOT NULL,
    received_at_ms INTEGER NOT NULL,
    confirmed_at_ms INTEGER NOT NULL,
    PRIMARY KEY (inspection_id, point_index)
);

CREATE TABLE outbox (
    event_id TEXT PRIMARY KEY,
    inspection_id TEXT NOT NULL REFERENCES inspection(inspection_id),
    event_type TEXT NOT NULL,
    payload TEXT NOT NULL,
    state TEXT NOT NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    request_id TEXT,
    last_error TEXT,
    created_at_ms INTEGER NOT NULL,
    accepted_at_ms INTEGER,
    UNIQUE(inspection_id, event_type)
);

CREATE TABLE actuation_log (
    id INTEGER PRIMARY KEY,
    inspection_id TEXT REFERENCES inspection(inspection_id),
    host_epoch INTEGER NOT NULL,
    command_seq INTEGER NOT NULL,
    command TEXT NOT NULL,
    status TEXT NOT NULL,
    acknowledged_at_ms INTEGER,
    detail TEXT
);
~~~

state、event_type 和错误枚举在代码与迁移中统一；补充工作件查询、待传扫描索引。event_type 首版为 InspectionCompleted，异常上报以后另行扩展。

### 三类事务

1. 创建任务：保存任务、身份和规则快照，再请求 MCU 开始。
2. 确认点位：插入一个已确认点，提交成功后提示完成；主键防止同一点被重复插入。
3. 完成判定：核对点数，在同一事务中更新结果、结束时间与状态，并插入稳定 event_id 的 Outbox。

提交失败则回滚并中止，禁止发送 PASS。未确认候选允许只在内存和诊断日志中存在；确认数据必须有持久化记录。

重启时找出未完成任务，标记 ABORTED/service_restart，保留已确认点。所有未获平台确认的待传事件恢复为可重试状态；不重新计算或篡改历史规则。

## 9. L5：MQTT 与 OneNET 上报

### P0 必须先验证

创建产品与设备，确认账号套餐和物模型限制。建立 InspectionCompleted 事件并用测试客户端完成一次发送与回复；保存 topic、字段类型、认证方式和实际响应样例。

首版使用 MQTT/TLS，认证、CA 和设备信息从配置/环境读取。OneNET token 过期时提供可更新方式，不能无限使用失效凭据重连。

### 业务数据与平台适配

Outbox 保存内部业务 JSON，OneNetAdapter 转换为平台要求的 OneJSON 格式。业务层不依赖系统 topic。

内部数据包括 event_id、inspection_id、station_id、workpiece_id、product_id、rule_version、result、required_points、measurements 和时间质量。

为减少首版平台模型复杂度，云端可以定义 point_1_um～point_5_um 五个数值字段，加 required_points；未使用点不发送。完整本地记录仍按测量点表保存。最终字段以 P0 实际验证为准，不能直接把内部 JSON 当成平台允许的任意载荷。

区分：InspectionCompleted 是物模型事件标识；event_id 是业务事件唯一编号；request_id 是请求回复关联编号。OneJSON 对 request_id 的格式限制在接入时核实。

### 上传状态与确认

~~~text
PENDING → 编码并发送 → WAIT_REPLY
                      ├── 平台成功回复 → PLATFORM_ACCEPTED
                      ├── 超时/断网 → RETRY_WAIT → 重试
                      └── 明确参数拒绝 → REJECTED
~~~

MQTT publish 调用返回或 PUBACK 不能代替 OneNET 的业务接口回复。只有请求关联正确且平台成功接受，才更新 accepted_at_ms。

重试保持 event_id 不变，生成符合要求的 request_id 并保存映射。晚到回复不能误确认另一条事件；失败与重启边界可能导致重复上报，平台自动业务去重未经验证。

采用有上限的退避与发送速率。网络失败不能阻塞检测业务；补传速率遵守账号消息额度。待传数据与日志达到磁盘阈值后停止新任务并提示，而非删除未上传事件。

首版验收是本地可靠保存、恢复重试和平台接受确认。若增加自建接收端，再以 event_id 唯一入库并回复持久化成功，形成云端幂等闭环。

## 10. 配置、时间与运行管理

~~~yaml
station_id: station01
instrument:
  type: ble
  device_name: N26Y06M0445
workstation:
  device: /dev/serial/by-id/REPLACE_WITH_ACTUAL_DEVICE
  baudrate: 115200
  parity: even
  slave_id: 1
  poll_ms: 100
  heartbeat_ms: 200
database:
  path: ./data/inspection.db
cloud:
  adapter: onenet
  tls: true
  product_id: REPLACE_ME
  device_name: REPLACE_ME
~~~

配置缺失、无效规则或数据库不可写时启动失败并给出原因。密钥不提交 Git，日志不输出完整 token。

steady_clock 用于窗口、超时与健康期限；system_clock 用于追溯时间。系统时间未同步时标记 time_quality，不伪装为可信时间；记录 boot/run 标识辅助解释异常时序。

日志包含任务、点位、candidate_id、command_seq、event_id、状态转移和异常原因。原始通知日志可控，设置轮转与容量。

开发先前台运行；闭环后提供 systemd 单元，验证专用用户对 BlueZ、串口和数据目录的权限。进程异常退出由 Restart=on-failure 恢复，内部死锁不能仅靠这一选项处理。

SIGTERM 经主循环/安全通知请求退出：停止新任务→保存当前任务中止→请求 MCU ABORT→停止采集与上传→结束线程。信号处理函数不执行数据库、线程 join 或复杂 C++ 操作。

## 11. 按顺序实施的检查表

- [ ] L0：Ubuntu 实机 BLE 行为录制；OneNET 测试事件与回复；冻结共同协议。
- [ ] L1：仪器适配、连接代次、事件队列与终端显示。
- [ ] L2：身份输入、规则校验、任务建立与 Mock 工位。
- [ ] L3：点位开启/确认、状态机、逐点落库、判定与中止。
- [ ] L4：真实 Modbus、epoch/token/ACK、按钮事件与应用心跳。
- [ ] L5：最终事务与 Outbox、OneNET 编码、回复关联、补传。
- [ ] L6：崩溃恢复、空间不足、进程退出、部署和报告。
- [ ] 后续：ARM 依赖、构建与实机回归。

## 12. 必要测试及验收数据

| 测试 | 判据 |
|---|---|
| 同值多点与连续通知 | 两次独立确认可保存同值；单窗口多通知不能自动补足点数 |
| 旧连接/窗口外数据 | 不归入活动点位，诊断可见 |
| 阈值边界 | 单位一致；等于 min/max 的判定符合规则 |
| START/APPLY_RESULT 重传 | 不重复开始或重复改变输出 |
| 业务线程停滞 | 串口线程仍活着也不能持续更新应用心跳 |
| 第三点后杀进程 | 重启能查到已确认点和中止原因 |
| 最终事务前后杀进程 | 不出现已完成结果无对应 Outbox |
| 断网与回复丢失 | 本地检测继续；待传可恢复；重复上报边界如实记录 |
| 数据库只读/磁盘满 | 不发送 PASS，任务明确中止 |

记录真实测量、回放与仿真次数，分别报告。24h 测试必须写明实际负载、重连次数、记录数量、资源趋势及异常；“运行了24h”本身不能证明数据一致性。

## 13. Linux 交付物

可构建 C++ 工程、示例配置、数据库迁移、OneNET 模型/请求回复样例、串口设备配置、systemd 单元、测试数据和报告。

README 提供当前 Ubuntu 环境、构建、设备检查、工件录入、点位操作、查询、云端验证与错误定位步骤。ARM 移植在后续完成并实测前保持待完成状态。
