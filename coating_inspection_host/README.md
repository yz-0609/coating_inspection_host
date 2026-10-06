# Linux 涂层质检服务

实现单工位、单仪器、1～5 点人工确认流程。正式服务为 C++17，采用真实 SQLite 和 libmodbus。
无硬件可以运行完整流程；真实仪器、真实 RS485、MCU 输出及 ARM 尚需单独验收。
OneNET 尚未接入，Outbox 保持 PENDING，不伪造上传成功。

## 1. 构建与测试

Ubuntu 22.04 所需开发包：

```sh
sudo apt install build-essential cmake pkg-config libglib2.0-dev libsqlite3-dev \
  libyaml-cpp-dev libmodbus-dev nlohmann-json3-dev python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

数据库目录自动创建；运行用户必须具备目录写权限。配置路径中的相对路径以配置文件所在目录解析。

当前开发环境已正式安装 libmodbus 3.1.6 与 nlohmann-json 3.10.5 系统开发包。
代码目录已改名为 `coating_inspection_host`，现有 `build`、`build-system` 和 `build-sanitize` 的构建配置已按新路径重新生成。也可使用以下标准构建命令：

```sh
cmake -S . -B build-system -DCMAKE_BUILD_TYPE=Debug
cmake --build build-system -j4
ctest --test-dir build-system --output-on-failure
```

下文示例中的 `build/inspection_service` 也可替换为 `build-system/inspection_service`。
编译不需要从 GitHub 动态下载源码。进行长稳测试时应固定二进制及其依赖，避免中途变更测试条件。

## 2. 无硬件交互演示

无需手工输入的完整五点演示（默认通过真实主站连接 PTY）：

```sh
python3 tools/demo.py --binary build/inspection_service --output reports/demo-first
```

可加 `--result fail` 或 `--result aborted` 验证失败/中止；每次使用新的输出目录。
目录保留 SQLite 与 summary.json，方便查询，不会伪造硬件或云端成功。

手工交互：

```sh
./build/inspection_service --config config/demo.yaml
```

等待 `sync_complete`，依次输入：

```text
sim present 1
reset
status
start WORKPIECE-001 demo5
arm
```

操作时等待 `command_done` 确认 RESET/START 成功，再开启窗口。
等待 `candidate` 日志，复制其 `candidate_id` 并输入：

```text
confirm 这里替换为候选编号
```

每点重新 `arm`，共确认 5 点。示例仪器每秒产生 150 μm、FE 合成测量。确认的每个点都会出现 `point_saved`。
最后出现 `inspection_completed`，再通过 `command_done` 检查现场结果命令是否得到确认。

```text
query WORKPIECE-001
outbox
export /tmp/inspection-events.json
sim present 0
sim present 1
start WORKPIECE-002 demo1
arm
```

`demo1` 是单点演示产品，阈值也是 100～200 μm，含边界。所有规则仅供演示，不代表生产工艺要求。
工件编号可用双引号包含空格：`start "编号 A" demo1`。复检使用相同工件编号，生成新的检测编号。

其他命令：`cancel` 中止当前检测；`reset` 只复位故障状态；`quit` 正常退出。
到位在检测中消失会中止任务；完成后工件正常离开会清除结果，为下一工件准备。

## 3. PTY 协议仿真

启动独立模拟器：

```sh
python3 tools/workstation_sim.py
```

模拟器输出 `ready`，包含 `/dev/pts/N` 路径。复制 demo 配置到同一个 config 目录，修改：

```yaml
workstation:
  type: modbus
  device: /dev/pts/N
```

保留实际 `ready` 返回的路径，不写死。服务使用完整 libmodbus 主站，与实际硬件使用同一路径。
在模拟器终端输入控制 JSON：

```json
{"present":true}
{"button":"action"}
{"button":"reset"}
{"reset_mcu":true}
{"drop_ack":true}
{"ack_delay_ms":500}
{"reject_opcode":3}
{"drop_responses":1}
{"old_ack":[999,123,1]}
{"old_ack":null,"drop_ack":false,"ack_delay_ms":0,"reject_opcode":0}
{}
```

空对象查询逻辑状态；`outputs` 中 bit0 是 PASS，bit1 是 FAIL，bit2 是 FAULT。
`{"disconnect":true}` 关闭模拟串口；`{"quit":true}` 退出模拟器。
`--scenario 文件` 支持 JSON 控制数组，每项使用 `at_ms` 指定启动后的执行时间。

PTY 不承载电气或真实奇偶校验，不能验证物理 8E1、t1.5/t3.5 或 DE 波形。
模拟器按请求长度和 CRC 组帧，其目的为验证主站、寄存器和业务协议。

## 4. 真实硬件路径

使用 `config/hardware.yaml`，替换真实 `/dev/serial/by-id/...` 路径及 BLE 设备名称。
默认 115200、8E1、从站地址1、协议版本0x0200。服务不同时打开旧串口库和 libmodbus。
示例假定 USB-RS485 转换器在硬件中自动控制方向；需要内核或自定义 RTS 控制的转换器要另行适配并实测。

运行前检查 BlueZ 系统 D-Bus、蓝牙控制器、串口组权限、仪器是否被其他应用占用，以及 MCU 寄存器实现。
真实配置关闭 `testing.allow_controls`，因此不能通过 `sim` 命令伪造测量或停滞。
没有设备时保持离线/故障状态，不自动切换为 Mock，不允许开始检测。

BLE 接入基线和整数接口修改记录见 [`vendor/thickness/PROVENANCE.md`](vendor/thickness/PROVENANCE.md)。
程序尚未重新进行 BLE 实机验证。退出时 BLE 线程受上游有界 D-Bus 调用期限影响，正在建立连接时停止可能比回放模式慢。

## 5. 故障注入与长稳

仅开启测试控制的配置支持：

```text
sim measure 150 FE
sim connection 0
sim connection 1
sim stall 2500
sim overflow
sim file_limit
sim raw "BB 02 18 00 ..."
```

`sim measure` 把输入编码为仪器帧，再执行组帧与 CRC；不直接绕过解析器生成合法候选。
`sim present/button/mcu_reset` 只适用于内存 Mock，PTY 模拟器使用自己的 stdin 控制通道。
`sim file_limit` 将当前测试进程的文件写入容量设为0，SQLite 随后收到真实介质写错误；此限制不可在该进程中恢复，退出后重启。

`INSPECTION_TEST_CRASH_AT=before_final_commit` 或 `after_final_commit` 定位事务崩溃边界，只有测试控制启用时生效。
SIGTERM 会保存中止、请求撤销输出、停止线程。SIGKILL 由 MCU 应用心跳超时兜底。

24小时长稳（新输出目录）：

```sh
python3 tools/soak.py --binary build/inspection_service \
  --duration 86400 --interval 30 --output reports/soak-24h
```

短负载检查可改为 `--duration 60 --interval 3`。报告记录实际时长、完成标志、PASS/FAIL/中止数量、资源趋势和一致性检查。
短负载通过不代表24小时已经通过；报告中的 `completed` 仅对应请求时长。

## 6. 学习与交付文档

- [`ARCHITECTURE.md`](docs/ARCHITECTURE.md)：线程、调用链、模块阅读顺序和关键业务解释。
- [`PROTOCOL.md`](docs/PROTOCOL.md)：寄存器、命令、确认和 MCU 对接条件。
- [`VERIFICATION.md`](docs/VERIFICATION.md)：真实执行的测试与待验证项目。
- [`deploy/inspection.service.example`](deploy/inspection.service.example)：systemd 部署模板。

日志为一行 JSON，`message` 使用中文，字段包括任务、点位、候选、命令序号和状态。
前台日志写 stdout；systemd 下由 journald 保存和轮转，按设备容量配置 journald 配额。
原始通知默认关闭，可用 `instrument.raw_notifications: true` 开启。stdout 堵塞同样会阻止业务健康推进，MCU 应按超时保护。
