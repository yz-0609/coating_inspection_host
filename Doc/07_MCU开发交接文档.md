# 工业涂层质检与追溯系统：MCU 开发交接文档

交接日期：2026-10-06（北京时间）  
文档版本：V1.0；对应工位协议：`PROTOCOL_VERSION = 0x0200`  
接收对象：STM32F4 / FreeRTOS 下位机固件开发人员  
交接基线：本仓库当前 Linux 实现、配置、协议模拟器和测试；原始计划书作为设计依据。

> 本文交接的是 MCU 侧的完整开发范围及与现有上位机的协作接口。当前仓库已有 Linux 软件，没有 MCU 固件目录。上位机软件闭环和协议仿真已具备；真实 BLE、真实 RS485、MCU GPIO / DMA / 看门狗和整机联调仍需实机验收。OneNET 尚未接入。

## 1. 接手后首先明确的目标

实现一个可以直接替换现有模拟工位的 STM32F4 固件：读取到位开关和两个按钮，提供 Modbus RTU 从站，执行带身份校验的完整业务命令，控制 PASS / FAIL / FAULT 指示，并在上位机失活、工件中途离开或 MCU 内部任务异常时主动撤销输出。

上位机负责“测量、任务、质量判定和追溯”；MCU 负责“现场输入输出、现场状态和独立保护”。MCU 不需要接收每个厚度值，也不需要复制 Linux 的多点采集流程。能正常读写串口只是通信层完成，收到写响应也不代表现场业务执行成功。

首版范围为：单工位、单 BLE 测厚仪、每件 1～5 个测点、人工确认、低压开关和三色灯演示。蜂鸣器是可选项；PLC 联锁、扫码枪、屏幕、远程控制和升级协议均未在当前接口中实现。

### 1.1 应优先阅读的现有材料

| 文件 | 接手用途 |
|---|---|
| [Linux 使用说明](../coating_inspection_host/README.md) | 构建、演示、硬件配置、测试控制入口 |
| [协议定义头文件](../coating_inspection_host/include/inspection/protocol.hpp) | 当前地址、状态、命令和字段宽度 |
| [协议编解码与确认](../coating_inspection_host/src/workstation/protocol.cpp) | 上位机如何解释寄存器、认定命令成功 |
| [工位主站实现](../coating_inspection_host/src/workstation/workstation.cpp) | FC04 / FC06 / FC10、轮询、心跳、重传与断线 |
| [独立 PTY 工位模拟器](../coating_inspection_host/tools/workstation_sim.py) | 当前协议逻辑参考，可用于对照；不能作为 STM32 驱动直接移植 |
| [业务状态机](../coating_inspection_host/src/domain/worker.cpp) | 同步、任务、按钮、结果提交和异常恢复 |
| [硬件配置](../coating_inspection_host/config/hardware.yaml) | 实际上位机默认通信与业务参数 |
| [MCU 原始实施计划](03_MCU具体实施计划书.md) | GPIO、DMA、RTU、任务健康与 IWDG 的设计要求 |
| [整体计划](01_整体项目计划说明书.md) | 系统目标和原始公共协议 |
| [验收记录](../coating_inspection_host/docs/VERIFICATION.md) | 历史软件测试证据及尚未验收的硬件项目 |

若文字计划与实现有差异，接入当前主站时先满足 `protocol.cpp` 的确认条件，并按本文第 12 节处理尚未冻结的细节。需要改变接口时同步修改 Linux、模拟器和测试，不能只改 MCU。

## 2. 系统结构与职责分工

```text
BLE 测厚仪 ── GATT 通知 ──> Linux 上位机
                              ├─ 工件编号 / 产品 / 规则 / 多点流程
                              ├─ SQLite：任务、测点、质量结果、动作记录
                              ├─ Outbox：本地待传事件
                              └─ 云适配接口：当前 disabled，OneNET 待开发
                                        │
                                 USB-RS485 / Modbus RTU
                                        │
                                        ▼
                              STM32F4 + FreeRTOS
                              ├─ 到位开关、ACTION、RESET
                              ├─ 工位状态、epoch / token / seq / ACK
                              ├─ PASS 绿灯、FAIL 红灯、FAULT 黄灯
                              └─ 应用心跳、任务健康、IWDG、输出覆盖门控
```

| 项目 | 上位机责任 | MCU 责任 |
|---|---|---|
| 测厚仪连接与解析 | BlueZ / GDBus、通知组帧、CRC、材料、整数厚度、重连代次 | 不接 BLE，不解析测厚仪协议 |
| 工件与检测身份 | 录入工件 / 产品，分配 `inspection_id` 与 `session_token` 并落库 | 保存当前非零 `session_token`，据此校验任务归属 |
| 点位采集 | 开窗、候选编号、人工确认、1～5 点、窗口超时 | 上报按钮事件；不自行计点、开窗或确认测量 |
| PASS / FAIL | 按规则判定，结果和 Outbox 原子提交后发送命令 | 校验命令身份、状态和健康后应用输出 |
| 现场输入 | 轮询去抖后的电平与锁存事件，决定业务含义 | GPIO、去抖、按下沿、事件序号、事件保持 |
| 现场状态 | 维护更细的检测阶段，并与 MCU 状态交叉核对 | 维护 BOOT / IDLE / READY / INSPECTING / COMPLETED / FAULT |
| 失联保护 | 业务健康才发变化心跳；故障时尽量发送 ABORT | 独立计时；不能等待主站通知后才撤销输出 |
| 内部健康 | Linux 业务线程健康戳、有界队列与期限 | 关键任务进度、覆盖门控、条件喂狗 |
| 追溯与云端 | SQLite、动作确认日志、Outbox；后续 OneNET | 不保存完整工件信息，不上传云端 |
| 硬件默认态 | 启动不恢复旧结果，重新同步 | 上电、复位、故障、失联时不恢复旧 PASS |

`FAIL` 表示质量不合格；`FAULT` 表示系统或现场条件异常；`ABORTED` 是 Linux 的检测中止记录。三者不能混用。例如 BLE 断连中止不应伪造为质量 FAIL。

## 3. 上位机已经完成的部分及交接限制

### 3.1 已实现的软件能力

| 模块 | 当前已经实现 | 与 MCU 对接的意义 |
|---|---|---|
| Linux 服务 | C++17、配置加载、前台 CLI、JSON 日志、线程生命周期 | 固件可以直接用现有程序联调 |
| 仪器适配 | BLE 真实接入路径、回放后端、帧解析、连接代次、候选事件 | 首次 RS485 联调可先用回放仪器隔离 BLE 因素 |
| 业务流程 | 录入编号、产品规则快照、START 确认、独立窗口、候选确认 | MCU 的 INSPECTING 覆盖整个多点过程 |
| 质量规则 | 点数 1～5；逐点 FE / NFE 与厚度上下界；上下界包含边界 | MCU 只接受最终 `ARG=1/2`，不重新判定 |
| 本地持久化 | `inspection`、`measurement`、`outbox`、`actuation_log`，WAL / FULL，短事务 | 现场动作与质量结果分别记录 |
| 结果提交 | 质量结论与完成事件在同一事务提交，再发送 APPLY_RESULT | 不允许 MCU 提前依据点数亮 PASS |
| 重启恢复 | 未完成任务中止；待确认动作变 UNKNOWN；不重放历史 PASS | MCU 重启也必须使旧 token 和输出失效 |
| Modbus 主站 | 真实 libmodbus RTU，FC04 快照、FC06 心跳、FC10 命令 | MCU 必须按同一寄存器布局实现 |
| 命令确认 | epoch + seq + ACK_STATUS + 当前状态 / token / 到位 / 输出 | 不能只返回 `ACK_STATUS=1` |
| 异常路径 | BLE 断连、离位、命令失败、数据库故障、停滞、退出等 | 通道可用时 ABORT；通道不可用时由 MCU 超时兜底 |
| 云接口 | Disabled 适配器与本地 Outbox，事件保持 PENDING | 当前不涉及 MCU 云端开发 |
| 测试工具 | 内存 Mock、独立 PTY 模拟器、单元 / 契约 / 进程集成测试、演示和长稳脚本 | 提供可复现的软件与协议参考 |

厚度在上位机以 `milli_um` 保存，例如 `150000` 表示 `150 μm`。该单位不进入当前工位寄存器协议。演示产品 `demo5` / `demo1` 为 5 点 / 1 点，FE、100～200 μm，仅是演示规则。

### 3.2 不能当成已完成成果的项目

- 当前没有 STM32 固件、实际引脚分配和接线图。
- 当前项目的 BLE 适配尚需重新验证真实仪器行为，尤其一次物理测量与通知、同值测量、重连旧值的关系。
- PTY 验证了主站和协议逻辑，不能验证真实奇偶校验、RS485 电气、RTU 静默间隔、DMA、DE 或 GPIO 延迟。
- OneNET 认证、TLS、物模型、上传和平台接受确认未实现；待传记录不能写成上传成功。
- ARM Linux 移植和整机掉电测试未验收。
- 历史普通 CTest、ASan / UBSan 记录为 3/3 入口通过，包含 31 个进程集成场景；这些均为 PC 软件测试。
- 当前 `coating_inspection_host/reports/soak-24h/report.json` 实际记录为 `completed=false`，约 `4799.962 s`、54 PASS / 53 FAIL / 53 中止。不能根据目录名或旧文档“运行中”的描述宣称 24 小时已通过。60 秒报告为 `completed=true`、20 个任务。

### 3.3 本次交接文档核对记录

2026-10-06 使用现有 `coating_inspection_host/build-system` 二进制复跑 CTest：unit、simulator_contract、integration 共 3/3 通过，总耗时 31.21 s。本次未重新构建二进制、未进行硬件验收。文档内相对文件链接已检查，附录 8 条 RTU 请求的长度和 CRC 已校验，SYNC / RESET / START / PASS / ABORT 调试序列已通过现有 PTY 行为模型核对。

## 4. MCU 必须交付的完整范围

| 编号 | 工作包 | 开发内容 | 完成证据 |
|---|---|---|---|
| M0 | 工程与硬件基线 | 具体 F4 型号、时钟、HAL / FreeRTOS、启动文件、链接脚本、引脚、DMA、工具链 | 可重复构建 / 烧录，接线表 |
| M1 | 现场输入输出 | 到位与按钮去抖、事件锁存、输出统一管理、默认电平 | 短按 / 长按 / 抖动、上电灯态实测 |
| M2 | UART / RS485 | DMA 环形接收、HT / TC / IDLE、新增区间、UART 错误恢复、DE / USART TC | 缓冲跨界与发送波形 |
| M3 | RTU 从站 | 定界、CRC、地址 / 功能 / 数量校验、FC03 / 04 / 06 / 10、标准异常 | 主站读写与畸形帧测试 |
| M4 | 业务协议 | 完整命令队列、状态机、epoch / token / seq、幂等、ACK、一致快照 | 正常命令及非法 / 重复命令测试 |
| M5 | 心跳保护 | 变化值计时、1500 ms 初始期限、锁存、故障覆盖 | 断线 / 杀主站 / 重复旧心跳时的 GPIO 撤销延迟 |
| M6 | 健康与看门狗 | 三任务进度、条件喂狗、IWDG、复位原因、内部异常 | 任务停滞复位与外部离线稳定故障态 |
| M7 | 整机联调 | 真实主站、回放仪器、真实仪器、异常恢复、结果一致性 | 带版本、日志、波形的测试报告 |

## 5. 物理通信与时间参数

### 5.1 必须兼容的通信配置

| 项目 | 当前值 / 要求 |
|---|---|
| 总线 | RS485 半双工，Linux 主站、MCU 从站，首版单从站 |
| 串口 | 115200 bit/s，8 数据位、偶校验、1 停止位，即 8E1 |
| 从站地址 | 1；主站配置可改，首版 MCU 默认与之相同 |
| 协议版本 | `0x0200`，输入地址 `0x0013` |
| 地址表达 | 下文均为协议中的零起始十六进制地址，不是 30001 / 40001 展示编号 |
| 寄存器字节序 | 单个 16 位寄存器高字节在前 |
| 32 位字序 | 低地址放高 16 位，高地址放低 16 位 |
| CRC | Modbus CRC16，初值 `0xFFFF`、反射多项式 `0xA001`，帧尾低 CRC 字节在前 |
| 广播 | 首版不接受地址 0 改变业务，不响应广播 |

例如 `0x12345678` 占用两个寄存器时为 `[0x1234, 0x5678]`，在线数据字节为 `12 34 56 78`。不能把 CRC 的低字节优先误套到寄存器上。

STM32 UART 配置应确认开启偶校验后实际仍有 **8 个有效数据位**；HAL 的 WordLength 设置需按具体系列和库版本核对，不只看配置界面上的名称。

### 5.2 已有主站参数与 MCU 初始设计参数

| 参数 | 数值 | 所属 / 解释 |
|---|---|---|
| 快照轮询 | 100 ms | Linux `poll_ms`，受实际事务耗时影响 |
| 变化应用心跳 | 200 ms | Linux `heartbeat_ms`，仅业务健康时发送 |
| 业务健康 / 快照可用期限 | 500 ms | Linux `business_health_ms`；不是 MCU 失联期限 |
| 单次串口响应期限 | 150 ms | Linux `response_timeout_ms`；MCU 响应必须有界 |
| 一条业务命令确认总期限 | 1000 ms | Linux `command_timeout_ms`，从出队待发送开始计时 |
| 额外重传次数 | 2 | 首次发送 + 最多 2 次重传；内容和 seq 不变 |
| 测量窗口期限 | 30000 ms | Linux 业务控制，与 RTU 无关 |
| 主站事务后间隔 | 至少 1750 μs | 当前主站软件等待；物理时序需实测 |
| MCU 主站心跳超时 | 初始 1500 ms | 当前模拟器和 MCU 计划值；需落实到固件常量 / 配置 |
| MCU WorkstationTask | 初始 10 ms | 采样 / 去抖 / 命令执行 |
| MCU HealthTask | 初始 100 ms | 健康检查与心跳保护 |
| 输入去抖 | 初始 30 ms | 建议起点，实测后调整，不是已冻结指标 |
| RTU t1.5 / t3.5 | 初始 750 / 1750 μs | 原 MCU 计划设计值，须定时实现和波形验证 |
| IWDG | 初始约 3 s | 按实际 F4、LSI 范围计算，最终报告实测 |

MCU 的 1500 ms、去抖和 IWDG 不能通过现有寄存器远程配置；若增加配置能力，需要另行定义接口。1000 ms 是命令整体确认期限，不是每次重传各有 1000 ms。1500 ms 的最终输出撤销时间还包含健康任务调度和 GPIO 更新耗时。

## 6. 完整寄存器接口

### 6.1 FC04 输入寄存器：一次读取 `0x0000` 起共 22 个

| 地址 | 字段 | 宽度 | 必须提供的含义 |
|---|---|---|---|
| `0x0000` | MCU_STATE | 16 位 | 0 BOOT / 1 IDLE / 2 READY / 3 INSPECTING / 4 COMPLETED / 5 FAULT |
| `0x0001` | INPUT_FLAGS | 16 位 | bit0 到位；bit1 ACTION 去抖电平；bit2 RESET 去抖电平；未用位清零 |
| `0x0002` | OUTPUT_FLAGS | 16 位 | bit0 PASS / bit1 FAIL / bit2 FAULT；未用位清零 |
| `0x0003` | FAULT_CODE | 16 位 | 0 无故障；故障表见第 10 节 |
| `0x0004` | HEALTH_MASK | 16 位 | 任务健康证据；各 bit 尚需冻结；Linux 当前只判断是否非零 |
| `0x0005` | MCU_HEARTBEAT | 16 位 | MCU 诊断心跳；推进语义尚需冻结，不能替代主站应用心跳 |
| `0x0006` | UPTIME_MS_H | 16 位 | MCU 本次启动单调运行毫秒数，高字 |
| `0x0007` | UPTIME_MS_L | 16 位 | 同上，低字；32 位正常回绕 |
| `0x0008` | ACTIVE_SESSION_TOKEN_H | 16 位 | 活动任务 token 高字，0 表示无任务 |
| `0x0009` | ACTIVE_SESSION_TOKEN_L | 16 位 | token 低字 |
| `0x000A` | ACK_CMD_SEQ_H | 16 位 | 最近业务确认命令序号高字 |
| `0x000B` | ACK_CMD_SEQ_L | 16 位 | 命令序号低字 |
| `0x000C` | ACK_STATUS | 16 位 | 0 无确认；1～6 见第 7 节 |
| `0x000D` | INPUT_EVENT_SEQ_H | 16 位 | 锁存按钮事件序号高字 |
| `0x000E` | INPUT_EVENT_SEQ_L | 16 位 | 事件序号低字 |
| `0x000F` | INPUT_EVENT_CODE | 16 位 | 0 无待处理事件 / 1 ACTION / 2 RESET |
| `0x0010` | RESET_REASON | 16 位 | 本次启动复位原因；编码需随所选芯片冻结 |
| `0x0011` | COMM_ERROR_COUNT_H | 16 位 | 累计诊断错误计数高字 |
| `0x0012` | COMM_ERROR_COUNT_L | 16 位 | 错误计数低字；分类和回绕策略需约定 |
| `0x0013` | PROTOCOL_VERSION | 16 位 | 固定 `0x0200` |
| `0x0014` | ACK_HOST_EPOCH_H | 16 位 | 最近 ACK 所属命令 epoch 高字 |
| `0x0015` | ACK_HOST_EPOCH_L | 16 位 | ACK epoch 低字 |

**一致快照要求：**在短临界区内复制完整状态，再从副本编码响应，禁止边发送边读取共享字段。state、token、output、fault、ACK 三字段及事件两字段必须来自同一个逻辑时刻，32 位值不得读到半新半旧。HealthTask 撤销输出和寄存器更新也要遵守该一致性要求。

Linux 当前只用 INPUT_FLAGS 的 bit0 判断到位，不用按钮电平推断事件。MCU 必须提供 `INPUT_EVENT_SEQ / CODE`，仅提供电平会遗漏短按。`OUTPUT_FLAGS` 应反映输出管理器实际应用的逻辑状态；现有硬件方案未定义独立灯具反馈，所以寄存器成功不能证明外部灯具或执行器物理动作。

### 6.2 FC06 心跳保持寄存器

| 地址 | 字段 | 写入与读取规则 |
|---|---|---|
| `0x0100` | HOST_HEARTBEAT | FC06 只允许写此地址；FC03 可读最近写入值 |

仅当新 16 位值与最近值不同，才更新 `last_host_health_tick`。相同值回写可以正常回复，但不能刷新健康期限；从 `0xFFFF` 到 `0x0000` 也是变化。不要求差值恰好为 1，允许中间值因丢帧未收到。一般读请求、FC10 命令和串口有数据都不能代替变化心跳。

### 6.3 FC10 命令保持寄存器

每次 **必须从 `0x0110` 起一次写 9 个寄存器，字节数 18**。FC10 是十六进制功能码 `0x10`，即十进制 16。

| 地址 | 字段 | 宽度 |
|---|---|---|
| `0x0110` / `0x0111` | HOST_EPOCH 高 / 低 | 无符号 32 位 |
| `0x0112` / `0x0113` | SESSION_TOKEN 高 / 低 | 无符号 32 位 |
| `0x0114` / `0x0115` | CMD_SEQ 高 / 低 | 无符号 32 位 |
| `0x0116` | OPCODE | 无符号 16 位 |
| `0x0117` / `0x0118` | ARG 高 / 低 | 无符号 32 位 |

不能用 FC06 写单个 OPCODE 或 token，也不能接受半个 FC10 命令。只有地址、数量、byte count、实际帧长和 CRC 都合法，才复制完整命令交给业务执行任务。FC03 建议与现有模拟器一致，允许读取 `0x0100` 单字和 `0x0110～0x0118` 连续子区间用于诊断；具体返回“最近已处理命令”还是“最近已接收命令”需冻结，不能作为执行成功依据。

### 6.4 Modbus 异常与业务拒绝分开

| 错误 | 通信层处理 |
|---|---|
| CRC 错误、非本机地址、无效 RTU 帧 | 丢弃，必要时计数，不改变输出 |
| 不支持功能码 | 标准异常 `0x01` Illegal Function |
| 寄存器地址 / 访问范围非法 | 标准异常 `0x02` Illegal Data Address |
| 数量为 0、越界、命令数量或 byte count 不符 | 标准异常 `0x03` Illegal Data Value；物理畸形帧按解析规则丢弃 |
| FC10 完整且合法，但 token / 状态 / 参数不允许 | 正常 FC10 响应，之后通过业务 ACK_STATUS 返回拒绝 |
| 内部命令队列已满 | 不得先响应成功再静默丢弃；建议异常 `0x06` Slave Device Busy，作为待冻结实现约定 |

所有范围校验应防止 `start + count` 整数溢出。MCU 支持 FC03 / 04 / 06 / 10；Linux 生产循环目前使用 FC04 / 06 / 10，不会通过 FC03 判断动作成功。

## 7. 六种业务命令、ACK 与身份规则

### 7.1 各标识的作用

| 标识 | 谁生成 | 作用与生命周期 |
|---|---|---|
| `inspection_id` | Linux UUID | 完整检测任务编号，只在 Linux 保存，不在寄存器发送 |
| `HOST_EPOCH` | Linux SQLite `metadata` 持久化递增 | 隔离主站启动 / 重新同步周期；MCU 当前 epoch 在 RAM 保存，复位后清零 |
| `SESSION_TOKEN` | Linux SQLite 持久化递增 | 非零 32 位任务编号；START 绑定，完成保留，离位 / 故障 / 同步清除 |
| `CMD_SEQ` | Linux 当前 epoch 内递增 | 区分单条命令；从 1 开始；重发不变，耗尽不能回绕复用 |
| `INPUT_EVENT_SEQ` | MCU | 区分按钮事件，与 CMD_SEQ 无关；同一待确认事件保持同一编号 |

不能通过厚度值、工件字符串或灯状态识别任务。当前 Linux 不在 MCU 写入产品规则、点数、工件编号或候选编号。

### 7.2 命令行为表

| OPCODE | 名称 | 当前 Linux 发送内容 | MCU 校验与成功后行为 |
|---|---|---|---|
| 1 | SYNC_SAFE | 新非零 epoch；token=0；ARG=0 | 建立新周期，清 token / 旧按钮 / 结果输出，FAULT，outputs=4，等待人工 RESET |
| 2 | START | 当前 epoch；新非零 token；ARG=0 | 仅 READY + 到位 + 心跳新鲜 + 关键任务健康时接受；绑定 token，INSPECTING，outputs=0 |
| 3 | APPLY_RESULT | 当前 epoch / 任务 token；ARG=1 PASS 或 2 FAIL | 仅 INSPECTING + 同 token + 到位 + 健康时应用；COMPLETED，token 保留，outputs=1 或 2 |
| 4 | ACK_INPUT_EVENT | 当前 epoch；ARG=待确认事件序号；token 可能为 0 或当前 / 保留任务 token | 对比事件序号且有锁存事件；清 EVENT_CODE 为 0，不改变工位结果 |
| 5 | RESET_FAULT | 当前 epoch；token=0；ARG=0 | 仅故障条件可解除、心跳新鲜、关键任务健康时接受；清 fault / token / 输出，按到位进入 READY 或 IDLE |
| 6 | ABORT | 当前 epoch；Linux 当前 / 保留任务 token，或无任务时为 0；ARG=0 | 有活动 token 时必须匹配；清 token / 结果输出，FAULT，outputs=4，fault=5 |

补充规则：

1. SYNC_SAFE 是允许切换 epoch 的命令。当前 PTY 模型要求新 epoch **大于**已建立 epoch；完全相同的 SYNC_SAFE 重传走幂等返回，不再次清现场。
2. RESET_FAULT 采用“仅 FAULT 允许”的设计，与原计划和 PTY 模型一致；内存 Mock 对此更宽松，不能据 Mock 行为允许检测中复位。
3. ACK_INPUT_EVENT 不应强制要求 task token 非零或与活动 token 一致。Linux 在无任务时发 token=0，也可能在完成 / 中止后携带保留任务 token；该命令绑定的是事件序号。
4. ABORT 在 MCU 已清 token=0 时，允许接收当前 epoch 内主站携带原任务 token 的撤销命令。否则离位或超时后主站补发 ABORT 会被错误拒绝。若 MCU 当前仍有另一个非零 token，必须拒绝不匹配的命令。
5. ABORT 不依赖新鲜心跳才能执行撤销；SYNC_SAFE 也必须能先进入撤销输出态。START / APPLY_RESULT / RESET_FAULT 则必须检查健康。
6. 原计划要求健康任务与输出门控比模拟器更严格。不能因为收到 APPLY_RESULT 就绕过故障覆盖。未用 ARG 必须约定合法值，当前 Linux 均发 0；严格参数校验需按第 12 节与测试一起冻结。

### 7.3 ACK_STATUS 定义

| 值 | 含义 | 典型情况 |
|---|---|---|
| 0 | 无确认 | 启动默认值；尚无完成的业务处理 |
| 1 | 已执行 | 执行成功；完全相同重传返回历史结果 |
| 2 | 状态拒绝 | IDLE 收 START、非 INSPECTING 收新结果、非 FAULT 复位 |
| 3 | token 拒绝 | START 的 token=0、结果 / 活动撤销 token 不匹配 |
| 4 | epoch / 序号拒绝 | 旧 epoch、旧 seq、相同 seq 不同内容 |
| 5 | 健康条件拒绝 | 主站心跳过期、关键任务不健康、禁止输出门控未解除 |
| 6 | 参数 / 命令拒绝 | 未知 OPCODE、非法结果、按钮序号不匹配 |

执行任务发布 ACK 时一次更新 `ACK_HOST_EPOCH + ACK_CMD_SEQ + ACK_STATUS`。拒绝时也报告本次请求的身份，避免主站等待到超时；拒绝不改变活动 token、结果输出或既有任务状态。当前主站把明确拒绝 / 状态不符记录为 FAILED，把超时 / 传输执行不确定记录为 UNKNOWN。

**ACK_HOST_EPOCH 是“确认所属请求的 epoch”，不是另设的当前周期寄存器。**Linux 使用 ACK epoch 的变化辅助识别重启。因此生产链路应保持单主站，不要并发使用另一调试主站发送旧周期命令；否则可能触发 Linux 重新同步。

### 7.4 Linux 判定成功的精确后置条件

所有命令首先必须满足：在线、协议版本 `0x0200`、ACK epoch 与 seq 完全匹配、`ACK_STATUS=1`。再核对：

| 命令 | `command_satisfied()` 还要求 |
|---|---|
| SYNC_SAFE | state=FAULT，token=0，outputs **恰好等于 4** |
| START | state=INSPECTING，同 token，到位=true，outputs **恰好等于 0** |
| APPLY_RESULT | state=COMPLETED，同 token，到位=true，PASS 时 outputs **恰好等于 1**，FAIL 时 **恰好等于 2** |
| RESET_FAULT | state=IDLE 或 READY，token=0，outputs **恰好等于 0** |
| ABORT | state=FAULT，token=0，outputs **恰好等于 4** |
| ACK_INPUT_EVENT | `INPUT_EVENT_CODE=0` |

不要在 OUTPUT_FLAGS 未用位写调试状态，否则主站的全字相等校验会失败。FAULT 时不是“全灯关闭”：结果灯关闭、黄灯逻辑位打开，即 `4`。BOOT 初始化的瞬时全灭可以存在，但 SYNC_SAFE 确认时必须已经达到上述状态。

### 7.5 幂等与重传的执行顺序

MCU 至少保存最近已处理命令的完整 9 字、身份及 ACK 结果。主站一次仅有一个等待确认的命令，所以最近命令缓存可覆盖当前重传场景。

```text
收到完整命令
  → 是否与最近命令 epoch / seq 相同？
      → 完整 9 字相同：返回原 ACK，不重新执行
      → 内容不同：ACK_STATUS=4，不改现场，不覆盖原幂等结果
  → 新 SYNC_SAFE：检查新 epoch，执行撤销与新周期建立
  → 其他新命令：检查当前 epoch、seq 前进、参数、token、状态、健康
  → 成功或明确业务拒绝：发布 ACK，维护本周期处理序号与幂等缓存
```

同 epoch 的新 seq 不要求刚好加 1，要求大于已处理 seq；合法新序号的拒绝也应按当前 PTY 模型缓存，重传不能因为后来条件改变突然执行。旧序号不得重新执行。缓存冲突处理不能破坏原始命令的结果。

最关键场景：APPLY_RESULT(PASS) 曾成功，随后发生心跳超时。收到相同命令时可以返回原 ACK_STATUS=1，但当前状态仍须 FAULT、token=0、outputs=4，不能再次开绿灯。Linux 会核对当前状态，拒绝把这个历史 ACK 当成当前 PASS。

## 8. 上下位机共同完成的流程

### 8.1 上电、上线与人工复位

```text
MCU 上电 → BOOT 初始化，结果输出关闭
         → 未同步 FAULT，fault=6，token=0，outputs=4，ACK 清零
Linux 启动 → SQLite 恢复 → 读快照并核对版本
           → 分配新 epoch → SYNC_SAFE → 读到匹配 ACK 与故障安全态
           → 日志 sync_complete，等待人工 reset
Linux 业务健康 → 工位线程持续发送变化心跳
操作员 CLI reset 或物理 RESET 事件
           → Linux 发送 RESET_FAULT
MCU 校验健康与故障可解除 → IDLE（无件）或 READY（有件），outputs=0
```

MCU 不能上电自动 READY 并保留旧输出。心跳与 SYNC 的具体到达先后受线程调度影响，不能要求必须先人工复位才开始收心跳，也不能让 SYNC_SAFE 依赖已经 READY。

### 8.2 开始一件检测

1. MCU 到位信号经去抖稳定；已同步且无故障时 IDLE → READY。
2. 操作员在上位机输入 `start 工件编号 产品编号`。
3. Linux 检查仪器在线、MCU 到位 / READY / 健康、快照新鲜、已同步、无活动任务与待处理命令、数据库可写。
4. Linux 分配新 inspection_id / token，保存任务与规则，进入 START_PENDING，然后发送 START。
5. MCU 验证并进入 INSPECTING，绑定 token，结果灯保持关闭，发布 ACK。
6. Linux 获得业务确认后将任务标为 INSPECTING，进入 WAIT_ARM；此时才允许采点。

**当前 ACTION 按钮不能从 READY 创建新任务。**代码只在 WAIT_ARM / WAIT_CONFIRM 解释 ACTION。工件和产品仍由 CLI 输入；如希望按钮启动“已预选任务”，需要新增上位机待启动上下文和测试，不能由 MCU 单独实现。

### 8.3 每一个测点如何协作

| Linux 阶段 | 操作 / 上位机行为 | MCU 行为 |
|---|---|---|
| WAIT_ARM | CLI `arm` 或 ACTION 事件开启新窗口 | 上报 ACTION，不改变 INSPECTING |
| WAIT_NEW_MEASUREMENT | 等待窗口后、同 BLE 连接代次的新测量 | 维持到位、心跳检查；此时 ACTION 无效但仍需事件确认 |
| WAIT_CONFIRM | 显示候选；CLI `confirm candidate_id` 或 ACTION 确认 | 只上报按钮事件，不生成 candidate_id |
| 点位保存成功 | 写入一个确认点；若不足点数回 WAIT_ARM | 持续 INSPECTING，不自行累加“已测点数” |
| 达到规则点数 | 最终判定并提交事务，之后发 APPLY_RESULT | 等待最终命令，不提前输出 PASS / FAIL |

ACTION 使用的是上位机快照入队时绑定的已展示候选编号；它不是 MCU 捕获按钮沿瞬间的仪器值。物理按钮到轮询存在延迟，当前协议没有按钮时间戳或候选编号字段。操作员仍需观察确认提示，不能假定物理按下时刻已经与特定 BLE 通知精确同步。

同样厚度可以在不同窗口成为不同点；连续通知不是多个已确认点。MCU 不应按测量通知次数或按钮次数替上位机判断完成。

### 8.4 按钮锁存与确认

```text
去抖后的按下沿 → 分配新事件序号，EVENT_CODE=1 或 2
              → 多次 FC04 均返回同一事件（即使按钮已松开）
Linux 按事件序号去重并处理业务
              → FC10 ACK_INPUT_EVENT，ARG=该事件序号
MCU 精确匹配 → EVENT_CODE=0，发布 ACK；事件序号可保留
```

- 长按不重复发事件；松开后再次按下才生成新事件。
- 首版只有一个待确认事件；占用期间新按键不得覆盖旧序号 / 类型，计入忙碌诊断或独立计数。
- 事件被业务拒绝也要确认清除，例如 WAIT_NEW_MEASUREMENT 的 ACTION、活动检测中的 RESET。
- 物理 RESET 是业务请求，不是 NRST，也不能直接清 FAULT / token。
- Linux 对 RESET 先排 ACK_INPUT_EVENT，再在无活动任务且 MCU 为 FAULT 时排 RESET_FAULT；两者由工位线程串行发送 / 确认。
- SYNC_SAFE 清旧待确认事件，防止旧按键在新任务重放；新同步后应继续识别新的事件。
- 按钮事件编号应在同一次 MCU 运行中避免复用。溢出与复位策略需冻结；Linux 同步会重置自身去重上下文。
- 当前主站的按钮 ACK 成功条件是读到 EVENT_CODE=0。若刚清旧事件就立即锁存下一事件，主站可能只看到新 CODE 而把旧 ACK 判为状态不符。快速连按 / 清事件与轮询交叉必须专项测试；事件再开放时机或上位机确认条件改进需双方明确，不能假定当前已解决。

### 8.5 完成、正常离位与下一件

Linux 最终事务提交 → 发送 APPLY_RESULT → MCU 校验后 COMPLETED / PASS 或 FAIL → Linux 确认现场动作。工件尚到位且主站健康时保持结果。

完成后正常离位：MCU 清 token / 结果输出，COMPLETED → IDLE；Linux 观察到 IDLE + 无件后释放当前任务上下文，历史数据库保留。下一件到位再进入 READY，重新创建任务，不复用 token。

质量结果已经落库但 APPLY_RESULT 超时 / 拒绝时，Linux 保留原质量 PASS / FAIL，记录现场执行 UNKNOWN / FAILED，进入故障流程并尝试 ABORT。不能把“数据库 PASS”直接解释为“现场绿灯已经确认”。

### 8.6 MCU 复位或通信恢复

MCU 复位应清当前 epoch、token、命令缓存与 ACK，uptime 从本次启动计时，结果输出关闭。Linux 通过 uptime 回退、BOOT 或 ACK epoch 变化识别复位，中止活动任务并请求新 SYNC_SAFE。

通信重新上线也重新同步；不接续旧采集窗口、不复放历史 PASS。恢复顺序为：原因排除 → 新 SYNC_SAFE（必要时）→ 人工 RESET → 新任务。Linux 主动 ABORT 产生的 fault=5 通常沿用当前 epoch 等待人工复位；外部故障 / 重启通常触发新 epoch。

## 9. MCU 状态机与输出规则

| 当前状态 | 触发与条件 | 下一状态 | token / 输出处理 |
|---|---|---|---|
| BOOT | 初始化结束，尚未同步 | FAULT | token=0，outputs=4，fault=6 |
| 任意状态 | 合法新 SYNC_SAFE | FAULT | 绑定新 epoch；清任务、结果、旧事件；fault=6 |
| FAULT | 合法 RESET_FAULT，原因可解除 / 健康新鲜 | IDLE 或 READY | token=0，outputs=0，fault=0 |
| IDLE | 到位稳定 | READY | 无任务，无结果输出 |
| READY | 到位消失 | IDLE | 无任务，无结果输出 |
| READY | 合法 START | INSPECTING | 绑定非零 token，outputs=0 |
| INSPECTING | 合法 APPLY_RESULT(1/2) | COMPLETED | 保留 token，outputs=1/2 |
| INSPECTING | 到位消失 | FAULT | 清 token，outputs=4，fault=2；无需等待主站 |
| COMPLETED | 工件正常离位 | IDLE | 清 token，outputs=0 |
| 已同步任意状态 | 主站变化心跳过期 | FAULT | 清 token，outputs=4，fault=1 |
| 可撤销状态 | 合法 ABORT | FAULT | 清 token，outputs=4，fault=5 |
| 任意运行状态 | 关键内部任务异常 / 严重内部故障 | FAULT / 看门狗恢复 | 禁止 PASS，撤销结果，保存诊断 |
| FAULT | 单纯恢复心跳 / 工件重新到位 | 保持 FAULT | 不自动解除锁存，不恢复结果 |

首版输出逻辑应只有五种组合：初始化 `0`；正常空闲 / 检测 `0`；PASS `1`；FAIL `2`；FAULT `4`。绿 / 红结果灯互斥，故障黄灯覆盖结果灯。GPIO 的高低有效电平由板卡记录定义，与逻辑位值分开。

建议 OutputManager 统一写 GPIO。HealthTask 设置禁止结果输出的锁存门控并立即撤销，普通输出写入也在短临界区内核对门控，防止“健康任务刚关绿灯、业务任务又开绿灯”。SYNC_SAFE 不解除允许正常输出的门控，只有合法 RESET_FAULT 才在健康条件满足时解除。

保护不依赖日志、串口响应或数据库；它们阻塞时也应先撤销输出。MCU 重启前后的硬件拉电阻、GPIO 初始化顺序与有效电平必须实测，保证软件尚未运行时没有旧 PASS。

## 10. 故障、应用心跳与看门狗

### 10.1 初始故障码

| 值 | 名称 | 触发 | 本期处理 |
|---|---|---|---|
| 0 | NONE | 无故障 | 正常流程 |
| 1 | HOST_TIMEOUT | 已同步后变化心跳超过期限 | 清任务和结果，FAULT 锁存 |
| 2 | WORKPIECE_REMOVED | INSPECTING 中到位消失 | 立即清任务和结果，FAULT |
| 3 | RX_BUFFER_OVERFLOW | 接收缓冲溢出 | 丢当前帧并重同步；活动检测进入故障 |
| 4 | TASK_UNHEALTHY | 关键任务进度不健康 | 覆盖输出；按设计停止喂狗恢复 |
| 5 | HOST_ABORT | Linux 撤销 | 清任务和结果，等待人工复位 |
| 6 | HOST_NOT_SYNCED | 上电 / SYNC_SAFE 后 | 等待有效心跳与人工复位 |

1 / 2 / 5 / 6 在现有模拟行为中已使用；3 / 4 来源于 MCU 计划，需在固件落实。故障优先级、首故障还是末故障、多个原因如何保留尚需冻结，建议额外诊断保存首故障和时间，不依赖单个寄存器保留全部历史。

**fault=5 被 Linux 特别识别为主动撤销后的预期状态，不能任意复用为其他错误。**同步 fault=6 只是安全同步等待确认，不能代替硬件仍未解除的内部故障；关键原因应在门控或诊断中持续保留，RESET 不能绕过。

### 10.2 必须独立执行的变化心跳保护

维护最近值、是否已看到变化值、最近接受变化值的单调时间。建立 epoch 后，未看到新鲜心跳也不能允许 START / RESET。HealthTask 用回绕安全的无符号时间差比较期限，期限超出后先撤销 GPIO，再更新故障诊断。

Linux 串口线程独立于业务线程存在。只有最近业务正常循环不超过 500 ms 且健康开关有效、协议版本匹配时，串口线程才发新的心跳计数。因此 MCU 必须检查“值变化”，仅看串口有通信会让业务卡死被掩盖。

GPIO 撤销延迟应从**最后一次变化心跳实际被 MCU 接受**起测，不从用户执行 kill 的时刻推算。记录最大值与采样条件，包含 HealthTask 周期及调度误差。心跳恢复后保持故障，禁止自动恢复原结果。

`MCU_HEARTBEAT` 是另一个方向的诊断值，当前 Linux 没有用其推进检测 MCU 任务活性，不能靠随意递增该字段代替 MCU 内部任务健康检查。

### 10.3 内部健康与 IWDG

- CommTask / WorkstationTask 在完成一次有界正常循环后推进进度，HealthTask 检查所有关键任务；健康点不能由 ISR 或无条件定时器代写。
- 没有串口流量时 CommTask 仍需有界醒来完成健康循环；不能把“Linux 离线”误判成通信任务死锁。
- HealthTask 完成检查且关键任务健康才喂 IWDG；自身卡住由 IWDG 兜底。
- 关键任务卡住时先尽可能撤销输出，再停止喂狗。HealthTask 已卡住时，输出默认态依靠复位硬件路径保证。
- Linux 长时间离线但 MCU 内部健康时，保持 FAULT 并继续条件喂狗，避免持续复位风暴。
- 开机先读取复位原因，再清硬件标志；IWDG / 掉电 / 软件复位后均不恢复旧任务和结果。
- 按实际 LSI 频率范围、启动耗时和容许恢复时间选择 IWDG 参数，报告配置及测量范围。

### 10.4 两侧异常协作表

| 场景 | Linux 行为 | MCU 必须行为 |
|---|---|---|
| BLE 断连 / 测量超时 / 操作取消 | 中止活动任务，保留确认点，可通信时 ABORT | 接受合法撤销，清结果与任务 |
| 数据库写失败 / 低可用空间 / 关键事件溢出 | 故障，停止健康推进，尝试保存中止 | 即使 ABORT 未到达，仍按心跳超时撤销 |
| 检测中工件离开 | 收到快照后中止，必要时重新同步 | 独立立即 FAULT，不等上位机 |
| 质量 FAIL | 质量 FAIL 落库后发 APPLY_RESULT(2) | COMPLETED，红灯；不当作内部故障 |
| Linux SIGTERM / quit | 有界收尾，保存中止 / 请求撤销，再停线程 | ABORT 或随后心跳超时撤销 |
| Linux SIGKILL / 主机掉电 / RS485 断线 | 可能不能发送任何命令 | 独立超时撤销 PASS / FAIL |
| Linux 业务挂起，串口线程继续 | 健康过期停止推进变化心跳 | 相同值或普通轮询不能延长健康 |
| MCU 复位 | 当前任务中止，新同步 / 人工复位 | epoch / token / ACK 清零，旧结果失效 |
| 结果写响应 / ACK 丢失 | 标记 UNKNOWN，保留质量结论，故障恢复 | 重传幂等；故障后绝不重开旧结果 |
| 单个 CRC 错误 | 单次传输异常或未获响应 | 丢帧计数，不无条件复位整个 MCU |
| 云端未配置 / 断网 | 本期事件留在本地 PENDING | 不影响本地现场协议与灯态 |

## 11. 固件架构、驱动与实施顺序

### 11.1 推荐工程组织

```text
mcu/                         # 当前尚不存在，MCU 实施时新增
├── Core/                    # 启动、时钟、IRQ、HAL 初始化
├── Drivers/                 # CMSIS / HAL
├── Middlewares/             # FreeRTOS
├── BSP/                     # 板卡 GPIO / UART / 定时器 / IWDG
├── Device/                  # RS485、DI、OutputManager
├── Module/                  # RingBuffer、RTU、寄存器、健康监视
├── Application/             # 工位状态机、命令、事件、故障
├── tests/                   # CRC / 编解码 / 状态机主机测试与向量
├── docs/                    # 接线、构建烧录、诊断、实机报告
└── CMakeLists.txt           # 或现有工程的可重复构建入口
```

先沿用已有 F4 工程，具体芯片确定后再分配引脚 / DMA stream / channel；不能照搬其他型号。建议三任务即可：

| 执行上下文 | 职责 | 禁止行为 |
|---|---|---|
| UART / DMA / Timer ISR | 提取有限新增数据、标记错误 / 定界、FromISR 通知 | 解析业务、开结果灯、阻塞日志、等待互斥量 |
| CommTask | RTU 解析 / 回复、FC06 健康输入、完整命令入队、快照编码 | 私自改任务状态，零散寄存器触发动作 |
| WorkstationTask | 输入去抖、按钮事件、统一业务状态、命令处理、ACK / 普通输出 | 越过故障覆盖门控 |
| HealthTask | 心跳期限、任务健康、输出覆盖、条件喂狗 | 不校验关键任务就持续喂狗 |

优先级由实测确定，须使健康撤销及时，同时防止通信洪泛占满 CPU。所有队列 / 环形缓冲有固定容量，超限可诊断。共享状态采用短临界区或合适同步机制；`volatile` 本身不保证原子性和一致性。

### 11.2 UART DMA / RTU / DE 必须实现的细节

1. 环形 RX DMA 处理 IDLE、半满、全满事件，记录上次位置，计算新增区间与跨环形段，避免同一数据重复搬运。512 字节 DMA / 1024 字节软件 RingBuffer 是原计划起点，最终按压力和调度验证。
2. UART IDLE 通常不是完整 RTU t3.5 静默期限。设计真实接收活动驱动的定时器或等效机制，不能在任务终于读取缓冲后才开始计时。
3. 定界后检查全部长度和 CRC，再解释字段。处理帧内超时、连续帧、接收跨界与噪声；单帧内存有上限。
4. UART ORE / FE / NE、DMA 错误和环形缓冲溢出须计数和恢复，不能一次错误后永久停 RX。
5. 完整响应准备好后置 DE → UART TX DMA → 等待 **USART TC** → 释放 DE。DMA 完成不表示最后停止位已经离开引脚。
6. 明确发送期间 RX / 本机回波处理，避免把自己的响应当请求；验证收发切换不会丢下一帧。
7. FreeRTOS FromISR 的中断优先级满足实际配置限制；调试日志走独立串口 / SWO / 非阻塞缓冲，不能污染 RS485。
8. RS485 终端、偏置、参考地、有效电平、USB 转换器方向控制方式和上电 DE 默认态记录在接线文档中。真实 24V IO 经接口电路接入。

### 11.3 分阶段推进与进入条件

| 阶段 | 完成后再进入下一步的条件 |
|---|---|
| A：硬件基线 | 能构建烧录；引脚 / 时钟 / 电平 / 输出默认态确认 |
| B：IO | 到位稳定、短按锁存、长按一次、故障输出覆盖实测 |
| C：串口与 RTU | Linux 正常读 22 字、写心跳；CRC / DE / 定界正确 |
| D：业务协议 | SYNC → 心跳 → RESET → START → RESULT / ABORT，ACK 与当前状态匹配 |
| E：独立保护 | 断线撤销、相同心跳无效、任务停滞 IWDG、健康离线无复位风暴 |
| F：整机回放 | 现有 C++ 主站 + 实际 MCU + 回放仪器完成 PASS / FAIL / 中止 |
| G：真实仪器 | 实际 BLE 仪器 + 实际 MCU 多点流程及异常测试 |
| H：交付 | 构建包、固件、接线、版本、日志、波形、测试报告完整 |

## 12. 联调前需要冻结的细节与现有实现差异

下表不是要求重设计全部协议。寄存器布局、版本、命令编号和主站后置条件已经明确；这些细项应在 MCU 设计记录中确认，涉及主站行为的同时补测试。

| 项目 | 当前事实 | 本次交接处理要求 |
|---|---|---|
| 板卡 / 引脚 / DMA | 只确定 STM32F4，具体型号未冻结 | 先出真实接线表；禁止假填引脚 |
| HEALTH_MASK | 模型健康值常为 3；Linux 只判断非零 | 定义关键任务 bit 及健康门槛；MCU 执行动作时检查全部关键条件，不能把任一 bit 非零当全健康 |
| MCU_HEARTBEAT | PTY 统计变化主站心跳次数；Mock 在心跳写入时递增；Linux 未检查其推进 | 冻结诊断含义；如要用作 MCU 自身健康指标，须新增主站验证，不能宣称当前具备 |
| RESET_REASON | 模拟值不是 STM32 硬件复位编码 | 定义位掩码或枚举，记录 RCC 原始标志与清除时机 |
| COMM_ERROR_COUNT | 模型也计未确认期间的新按键忙碌 | 定义累计口径与是否含按钮忙碌，必要时另留细分诊断 |
| RESET_FAULT 条件 | PTY 要求 FAULT，Mock 未严格限制；Linux 正常操作要求 FAULT | 固件实现仅 FAULT 复位并保留健康校验 |
| APPLY_RESULT 健康 | PTY 查心跳和 health；Mock 结果分支只查心跳 | 固件遵循 MCU 计划：关键任务、到位、门控、心跳都满足才输出 |
| 参数与 token 校验 | PTY 对 SYNC token / ARG 校验更严格；多种命令未用参数未完全校验 | 固件按命令表执行；ACK_INPUT 不要求非零 task token；新增严格拒绝规则补双方测试 |
| SYNC epoch 单调 | PTY / Mock 拒绝新 epoch 小于或等于当前值 | 保留 Linux metadata，换 / 回滚数据库不能直接复用旧计数；维护恢复方案需双方记录，不能随意放宽旧 epoch |
| 新 epoch 后心跳 | 模型 SYNC 不清既有心跳时间，MCU 计划未明确跨周期是否失效 | 建议新周期重新要求变化心跳；原健康未刷新前拒绝 RESET / START，联调验证上线时序 |
| 按钮连续事件 | Linux ACK_INPUT 要求 CODE=0，新事件可能覆盖该可观察状态 | 明确事件再开放时机或改进主站确认逻辑；快速连按专项验收 |
| 事件序号回绕 | 模型 32 位递增，最终行为尚未冻结 | 同一次运行避免复用；到极限前受控处理，确保新同步不会重放旧事件 |
| FC03 命令读回 | 模型读最近命令缓存，生产主站不使用 | 冻结读回语义；不把读回当业务 ACK |
| 命令入队失败 | 原计划未冻结异常方式 | 建议 Slave Device Busy；必须禁止成功回复后静默丢弃 |
| 故障优先级 / 复位条件 | 计划有故障码，模拟器保护逻辑可覆盖旧码 | 记录首故障 / 当前原因及解除规则，fault=5 仅主站撤销 |
| 物理时序与期限 | 有初始参数，没有实机证据 | 报告实际 RTU、DE、去抖、GPIO 撤销、IWDG 时间 |

上述建议不表示已改 Linux 代码。当前主站在 RESET / START 明确拒绝后不会无限自动等健康重试，因此首次同步后应等待变化心跳已经到达，再人工复位；若因健康拒绝，应重新确认条件后操作。

## 13. 可直接执行的上位机联调方法

所有命令在仓库的 `coating_inspection_host/` 目录执行。串口使用实际稳定路径，例如 `/dev/serial/by-id/...`，不要把示例占位符直接运行。确保无第二个串口主站占用设备。

### 13.1 构建与协议基线检查

```sh
cmake -S . -B build-system -DCMAKE_BUILD_TYPE=Debug
cmake --build build-system -j4
ctest --test-dir build-system --output-on-failure
```

依赖安装见 [Linux README](../coating_inspection_host/README.md)。可以先运行自动五点演示熟悉日志：

```sh
python3 tools/demo.py --binary build-system/inspection_service --output reports/mcu-handoff-pass
```

此命令默认使用 **PTY 模拟工位**，不是实际 MCU 验收；输出目录每次使用新名称。现有 simulator_contract / integration 可作协议参考，但不能直接把模拟器控制项当 MCU 寄存器命令。

### 13.2 第一轮：实际 MCU + 回放仪器

将硬件示例复制到同一配置目录，保留原文件：

```sh
cp config/hardware.yaml config/mcu_uart.yaml
```

修改 `config/mcu_uart.yaml` 中对应项，其余产品和参数保留：

```yaml
instrument:
  type: replay
  device_name: N26Y06M0445
  replay_file: replay.jsonl
  loop: true
  interval_ms: 1000
workstation:
  type: modbus
  device: /dev/serial/by-id/REPLACE_WITH_ACTUAL_DEVICE
  baudrate: 115200
  parity: even
  slave_id: 1
  poll_ms: 100
  heartbeat_ms: 200
  business_health_ms: 500
  response_timeout_ms: 150
  command_timeout_ms: 1000
  max_retries: 2
database:
  path: ../data/mcu-uart.db
  busy_timeout_ms: 100
  min_free_mb: 64
testing:
  allow_controls: true
```

这里是原配置的对应段替换，避免添加第二个同名 YAML 顶层字段。串口必须替换为实际路径。该文件放在 `config/`，所以 `replay.jsonl` 和相对数据库路径能按配置文件目录正确解析；数据库保留运行后产生的 epoch / token 计数。

```sh
./build-system/inspection_service --config config/mcu_uart.yaml
```

按下列步骤执行；每条操作后观察日志，不连续粘贴整套命令：

1. MCU 黄灯、结果灯灭；等待 Linux `sync_complete`，确认 SYNC_SAFE 的 `command_done.status=ACKNOWLEDGED`。
2. 等待主站变化心跳到达 MCU；操作实体到位开关，然后输入 `reset`。等待 RESET_FAULT 确认；`status` 应显示 READY、到位、仪器在线、已同步。
3. 输入 `start MCU-001 demo5`，等待 `inspection_started` 和 START 的成功确认。
4. 输入 `arm`，或按实体 ACTION；等 `window_armed`。等待回放测量产生 `candidate`。
5. 输入 `confirm 日志中的candidate_id`，或按实体 ACTION；等 `point_saved`。再开下一窗口，共确认 5 点。
6. 等 `inspection_completed.result=PASS`，还必须等 APPLY_RESULT 的 `command_done.status=ACKNOWLEDGED`。核对 MCU 绿灯、红黄灯灭、COMPLETED、任务 token 一致。
7. 输入 `query MCU-001` 和 `outbox`，核对 5 个点、质量结果、动作确认及本地 PENDING 事件。
8. 关闭到位开关，核对实际结果灯熄灭、MCU IDLE / token=0；重新到位再检测下一件。

先用 CLI 完成主线，再用实体 ACTION / RESET 替换对应步骤，方便分别定位业务与按钮问题。实际 Modbus 后端不接受 `sim present` / `sim button` / `sim mcu_reset`，这些只属于内存 Mock；实际 MCU 输入用实体开关或固件调试注入。

FAIL 测试可在一个窗口内用 `sim measure 250 FE` 注入并显式确认该条候选编号；仍要完成规则规定点数。该命令走回放仪器帧编码 / 解析路径，需 `allow_controls=true`。也可注入 100 / 200 边界或 NFE 材料确认规则。不要误确认随后自动回放的 150 μm 候选。

### 13.3 第二轮：实际 MCU + 真实 BLE 测厚仪

另复制硬件配置到 `config/mcu_hardware.yaml`，设置真实 BLE 名称、串口、独立数据库路径；保持 `instrument.type=ble`、`workstation.type=modbus`、`testing.allow_controls=false`，然后运行：

```sh
./build-system/inspection_service --config config/mcu_hardware.yaml
```

检查 BlueZ 系统 D-Bus、蓝牙控制器、串口权限、仪器占用和连接状态。无硬件时服务保持离线 / 故障，不自动退回 Mock。每个点开启新窗口后实际产生测量，再人工确认；记录物理测量次数、通知次数与候选归属。

### 13.4 故障注入与应保留的证据

- 实体断开 RS485、关闭主站、终止服务、MCU NRST、检测中离位：保存 Linux JSON 日志和 MCU 原因 / 状态，测 GPIO 关闭延迟。
- 回放联调配置可输入 `sim stall 2500` 验证业务挂起时串口线程仍在但变化心跳停止；恢复后必须同步 / 人工复位，不恢复旧 PASS。
- `sim connection 0` 测回放仪器断连中止；真实 BLE 使用实际断连。
- UART 错误、DMA 溢出、CommTask / WorkstationTask / HealthTask 停滞使用 MCU 调试版本定向注入，并明确标记测试固件。
- 原始帧 / 重复命令测试应先停止 C++ 服务，使用独立主站单独执行，再重新启动服务同步。不得两个主站同时操作总线。
- Linux 长稳工具当前使用 PTY 工位；实际 MCU 长稳需要单独建立硬件运行方案，不能把 PTY 报告改名作为 MCU 成果。

## 14. MCU 侧验收清单

每项记录固件版本、板卡、主站版本 / 配置、操作步骤、预期、实际、证据与结论。纯逻辑主机测试和上板测试分开。

| ID | 场景 | 必须达到的结果 / 证据 |
|---|---|---|
| T01 | 上电 / NRST / IWDG 复位 | 旧 PASS 不恢复；token / ACK 失效；默认电平和复位原因可查 |
| T02 | FC04 全快照和分段读取 | 22 字布局正确；32 位高低字正确；状态和 ACK 不撕裂 |
| T03 | FC03 / 06 / 10 正常及非法访问 | 正常响应 / 标准异常符合约定；零散命令写入无效 |
| T04 | CRC / 长度 / 数量 / 未知功能 / 非本机地址 | 丢弃或异常；不改变任务和结果输出 |
| T05 | UART 分片、环形跨界、HT / TC / IDLE、连续帧 | 无重复搬运、无误合帧；RTU 活动计时正确 |
| T06 | UART ORE / FE / NE、RX 溢出 | 可诊断且 RX 可恢复；活动检测按设计撤销 |
| T07 | DE 与 USART TC | 波形显示最后停止位后释放 DE、下一请求可收 |
| T08 | SYNC → 心跳 → RESET → START → PASS / FAIL | ACK 与主站精确后置条件匹配；灯态与 token 正确 |
| T09 | 无件 START、token=0、旧 token、旧 epoch、非法 ARG | 明确拒绝，现场不改变 |
| T10 | 同 seq 同内容 / 同 seq 不同内容 / 旧 seq | 原结果幂等返回 / 冲突拒绝 / 不重执行 |
| T11 | 故障后重发历史 PASS | 可回原 ACK，但仍 FAULT / outputs=4，绿灯不得重开 |
| T12 | 短按、长按、接触抖动 | 一次按下一个事件；松开后待确认事件仍可读 |
| T13 | 未确认期间新按键、旧事件 ACK、快速连按 | 不覆盖旧事件、不误清新事件；ACK 可正确被主站观察 |
| T14 | READY 时 ACTION、测量等待时 ACTION、检测中 RESET | 由 Linux 拒绝并确认事件；MCU 不擅自开始 / 计点 / 复位 |
| T15 | 检测中离位 / 完成后离位 | 前者独立故障撤销；后者清结果正常 IDLE |
| T16 | 相同 16 位心跳持续写 / 心跳正常回绕 | 旧值不能续命；FFFF→0000 视为变化 |
| T17 | 绿灯保持时断线、SIGKILL、主机掉电 | 从最后有效心跳到 GPIO 关闭的延迟有实测值，故障锁存 |
| T18 | Linux 业务停滞、通信线程继续 | 即使仍有总线活动也按变化心跳超时撤销 |
| T19 | CommTask / WorkstationTask / HealthTask 停滞 | 条件喂狗失效，IWDG 恢复，原因可查 |
| T20 | Linux 离线但 MCU 内部健康 | 长时间保持 FAULT，不产生重启风暴 |
| T21 | 活动检测中 MCU 复位、通信恢复 | Linux 中止旧任务，新 SYNC + 人工复位，无旧输出恢复 |
| T22 | 结果执行后响应 / ACK 丢失 | 幂等；主站记录 UNKNOWN 或故障，质量结果与现场动作区分 |
| T23 | HealthTask 撤销与 APPLY_RESULT 并发 | 故障门控不可被普通输出写入覆盖；临界区和 GPIO 证据 |
| T24 | 真实仪器 1 / 5 点、同厚度多点、边界、材料 FAIL | Linux 记录和 MCU 最终输出一致，候选未自动计点 |
| T25 | 整机断电 / 重启与硬件连续负载 | 历史本地记录可解释；旧灯不恢复；独立记录实际时长与错误 |

## 15. 交付物与完成定义

MCU 开发完成时应提交：

- 完整源码和依赖版本；HAL / FreeRTOS 配置、启动文件、链接脚本、工具链版本和一条可重复构建命令。
- ELF、BIN / HEX、MAP；固件版本、协议版本及编译选项，SWD 调试 / 烧录步骤。
- 板卡与芯片型号、时钟、USART、DMA、定时器、IRQ 优先级、GPIO 有效电平和实际接线图。
- 本协议的固件实现对应表，包括健康 bit、复位原因、错误计数、故障优先级和第 12 节冻结结论。
- 主机逻辑测试向量和上板验收报告，保留 Linux 日志、MCU 诊断、DE / TC 波形、默认态及输出撤销时间。
- 实际 MCU + 回放仪器的 PASS / FAIL / 中止证据，以及实际 MCU + 真实测厚仪的完整多点演示。
- 已知问题和待验收项，注明是否需要同步修改 Linux；不得用“模拟已通过”替代物理指标。

完成定义：现有 Linux 主站无需伪造现场状态即可驱动实际 MCU 完成检测；按钮、电平、命令身份、ACK 与现场状态一致；上位机失活、检测中离位和内部任务卡死均有独立保护证据；恢复后必须经过正确同步 / 人工复位，不会恢复旧 PASS。

## 附录 A：寄存器与 RTU 调试向量

以下向量用于独立主站和 MCU 协议测试，**不与运行中的 C++ 服务混用**。采用从站 1，epoch=1，token=99（`0x00000063`），seq 按示例递增；仅在 MCU 尚未绑定更高 epoch 的测试环境适用。SYNC 前后持续发送变化心跳，先置到位，再 RESET / START；若等待超过心跳期限，必须重新恢复条件。

### A.1 基础请求

```text
FC04 读 0x0000 起 22 字：
01 04 00 00 00 16 71 C4

FC06 写 HOST_HEARTBEAT=1：
01 06 01 00 00 01 49 F6
```

FC04 正常响应的数据字节数应为 `0x2C`（44），总帧长 49 字节。FC06 正常响应回显合法请求；上述心跳=1 反复写入不能维持健康。

### A.2 九寄存器命令内容

顺序：`epoch_H epoch_L token_H token_L seq_H seq_L opcode arg_H arg_L`。

```text
SYNC_SAFE    ：0000 0001 0000 0000 0000 0001 0001 0000 0000
RESET_FAULT  ：0000 0001 0000 0000 0000 0002 0005 0000 0000
START        ：0000 0001 0000 0063 0000 0003 0002 0000 0000
APPLY_PASS   ：0000 0001 0000 0063 0000 0004 0003 0000 0001
APPLY_FAIL   ：0000 0001 0000 0063 0000 0004 0003 0000 0002
ABORT        ：0000 0001 0000 0063 0000 0005 0006 0000 0000
```

PASS 与 FAIL 是两个独立场景，不能在同一任务上用相同 seq 依次发两个不同结果；第二条应被视为命令冲突。若实际流程插入 ACK_INPUT_EVENT 等命令，后续 seq 也相应递增。

### A.3 完整 FC10 请求（含 CRC）

```text
SYNC_SAFE：
01 10 01 10 00 09 12 00 00 00 01 00 00 00 00 00 00 00 01 00 01 00 00 00 00 BE FD

RESET_FAULT：
01 10 01 10 00 09 12 00 00 00 01 00 00 00 00 00 00 00 02 00 05 00 00 00 00 7C 3D

START：
01 10 01 10 00 09 12 00 00 00 01 00 00 00 63 00 00 00 03 00 02 00 00 00 00 28 78

APPLY_PASS：
01 10 01 10 00 09 12 00 00 00 01 00 00 00 63 00 00 00 04 00 03 00 00 00 01 A2 B8

APPLY_FAIL（另一个测试场景）：
01 10 01 10 00 09 12 00 00 00 01 00 00 00 63 00 00 00 04 00 03 00 00 00 02 E2 B9

ABORT：
01 10 01 10 00 09 12 00 00 00 01 00 00 00 63 00 00 00 05 00 06 00 00 00 00 BF B8
```

FC10 请求共 27 字节，正常写响应共 8 字节，只回地址、功能码、起始地址、数量与 CRC。随后必须 FC04 读取业务 ACK 和当前状态。CRC 基础黄金向量：`01 03 00 00 00 0A` 的 CRC 为 `0xCDC5`，帧尾发送 `C5 CD`。

## 附录 B：联调日志应如何解释

| Linux 日志 / 数据 | 证明什么 | 仍需核对什么 |
|---|---|---|
| `command_transmit` | 某身份命令已发送并等待确认 | MCU 是否执行、ACK / 后置条件 |
| `sync_complete` | SYNC_SAFE 业务确认通过 | 变化心跳、人工 RESET |
| `inspection_started` | START 确认后任务已开始 | 实体到位、后续测点流程 |
| `candidate` | 合法新测量成为当前窗口候选 | 人工确认，不能当已保存点 |
| `point_saved` | 一个确认点已提交 | 是否达到规则点数 |
| `inspection_completed` | 质量结果与待传事件已提交 | APPLY_RESULT 现场确认 |
| `command_done.status=ACKNOWLEDGED` | 该命令身份、ACK 和现场快照符合条件 | 实际 GPIO / 灯具物理行为需实测 |
| `command_done.status=UNKNOWN` | 当前无法证明现场是否执行 | 撤销、重新同步，不强行视为未执行 |
| `inspection_aborted` | 活动任务中止，确认点保留 | MCU 撤销输出确认或心跳保护 |
| `outbox.state=PENDING` | 本地事件待传 | 本期没有平台接受证据 |

联调报告应同时关联 inspection_id、session_token、host_epoch、command_seq、ACK、MCU state / fault / outputs 和固件版本。这样才能区分“业务完成”“现场执行确认”和“实际输出动作”。
