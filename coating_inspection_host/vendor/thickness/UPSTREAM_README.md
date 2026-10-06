# BLE 测厚仪 Linux 接收与解析程序

本项目用于在 Linux 设备上发现并连接 BLE 测厚仪，订阅实时测量通知，完成协议组帧、
CRC 校验和厚度解析，并将结果输出到终端或通过 C++ 回调交给上层业务。

当前程序已在 Ubuntu 22.04 x86_64 和实机 `N26Y06M0445` 上验证，适合直接运行在
Intel NUC，也可使用相同源码构建到采用 Debian/Ubuntu 的香橙派 aarch64 系统。
香橙派仍需根据具体板型确认蓝牙芯片、天线、内核驱动和系统镜像中的 BlueZ 状态。

## 1. 功能概览

- 按设备名称、Alias 或 public MAC 自动查找 BLE 测厚仪。
- 通过 BlueZ 系统 D-Bus 建立 GATT 连接，不依赖桌面蓝牙界面。
- 自动发现 `FFE0` 服务并订阅 `FFE1` 通知。
- 解析实时测量命令 `0x02`，输出 μm、mm、材料类型和组名。
- 校验 CRC-8，错误帧不会作为合法测量值交给业务层。
- 支持 BLE 数据拆包、粘包、前导噪声和错误后的重新同步。
- 设备断电、超距或连接中断后按退避策略自动重连。
- 提供纯 C++17 协议库和 BlueZ 回调库，方便嵌入其他服务。
- 提供原始 GATT 通知回调，为后续增加数据组 `0x03` 或厂商命令预留入口。
- 提供 CMake 安装导出、集成示例、内置自测和 systemd 服务模板。

当前未实现：

- 数据组命令 `0x03` 的业务解析和展示。
- 通过 `FFE2` 向设备发送控制命令。
- 数据库存储、CSV/JSON 输出、网络上报或图形界面。
- Windows、macOS、Android 和 iOS 蓝牙后端。

## 2. 设备与蓝牙说明

### 2.1 已验证设备

| 项目 | 当前实机信息 |
| --- | --- |
| BLE 名称 | `N26Y06M0445` |
| 实机 public MAC | `48:87:2D:7F:02:91` |
| 自定义服务 | `0000FFE0-0000-1000-8000-00805F9B34FB` |
| 实时通知特征 | `0000FFE1-0000-1000-8000-00805F9B34FB` |
| 写入特征 | `0000FFE2-0000-1000-8000-00805F9B34FB` |
| 配对要求 | 当前设备不要求预先配对 |

MAC 地址属于当前实机，不应写死到批量部署镜像中。程序默认按名称查找，也允许用
`--device` 指定另一台设备的名称或 MAC。

`FFE1` 支持 read、write、write-without-response 和 notify，本项目用它接收实时数据。
`FFE2` 支持 write 和 write-without-response，当前只保留设备信息，程序尚未向其写入。

这是一台 BLE GATT 设备，不是经典蓝牙 SPP 串口设备。在桌面“蓝牙设置”中显示
未配对、连接后又断开，不能直接说明设备故障。正确使用方式是应用建立 GATT 连接并
持续订阅 `FFE1`；应用退出后释放连接是正常行为。

### 2.2 通信流程

```text
扫描设备名称/MAC
        ↓
连接 org.bluez.Device1
        ↓
等待 ServicesResolved
        ↓
定位 FFE0 服务与 FFE1 特征
        ↓
StartNotify
        ↓
原始通知回调 → 流式组帧 → CRC-8 → Measurement 回调/终端输出
        ↓
断线时自动扫描并重连
```

## 3. 实时测量协议

原始协议文件为 [新款蓝牙协议b..25.pdf](新款蓝牙协议b..25.pdf)。本节记录程序实际
采用且已通过实机数据验证的解释。

### 3.1 帧结构

| 字段 | 长度 | 说明 |
| --- | ---: | --- |
| Flag | 1 字节 | 固定为 `0xBB` |
| CMD | 1 字节 | 实时测量为 `0x02` |
| Size | 2 字节 | 小端；包含 CMD、Size 自身和 Data |
| Data | n 字节 | 由命令决定 |
| CRC | 1 字节 | 覆盖 CMD、Size 和 Data，不包含 Flag |

实时测量帧中：

- Size 实测为 `18 00`，即 `0x0018 = 24`。
- Data 长度为 21 字节。
- 完整帧长度为 `1 + 24 + 1 = 26` 字节。
- 协议 PDF 示例写成了 `08 00`，与字段定义及实机报文不符，应视为文档笔误。

一帧已验证的原始数据：

```text
BB 02 18 00 64 44 16 00 00 44 65 66 61 75 6C 74 00 00 00 00 00 00 00 00 00 85
```

对应结果：

```text
厚度: 1459.3 μm (1.4593 mm), 材料: NFE, 组: Default, CRC: OK
```

### 3.2 Data 布局

| Data 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 4 字节 | 厚度原始值，小端、符号-幅值格式 |
| 4 | 1 字节 | 材料类型 |
| 5 | 16 字节 | 组名，尾部通常用 `0x00` 填充 |

厚度换算：

1. 将 4 字节按小端合并为 `uint32_t`。
2. 最高位为符号位：0 为正，1 为负。
3. 其余 31 位为幅值。
4. 幅值除以 1000 得到 μm，再除以 1000 得到 mm。

材料编码：

| 原始值 | 含义 |
| --- | --- |
| `0x00` | NFE |
| `0x01` | FE |
| `0x02` | 空 |
| 其他 | `unknown`，同时保留原始 `material_code` |

CRC 使用初值 `0x00`、多项式 `0x07`，按每字节最高位优先迭代。公共函数
`thickness::crc8()` 与协议 PDF 中的算法一致。

## 4. 项目结构

```text
.
├── app/main.cpp                         # 终端程序、参数处理和输出格式
├── include/thickness/
│   ├── bluez_reader.hpp                 # BlueZ 公共配置、状态与回调接口
│   └── protocol.hpp                     # 纯 C++ 协议数据类型与流式解析接口
├── src/
│   ├── bluez_reader.cpp                 # GDBus 扫描、连接、通知与重连实现
│   └── protocol.cpp                     # 帧同步、CRC 和实时测量解析
├── examples/callback_consumer.cpp       # 嵌入其他 C++ 程序的最小示例
├── docs/INTEGRATION.md                  # 更聚焦的集成与部署说明
├── deploy/thickness-reader.service.example
│                                         # systemd 服务模板
├── cmake/ThicknessReaderConfig.cmake.in # 安装后的 CMake 包配置
├── CMakeLists.txt
└── 新款蓝牙协议b..25.pdf                 # 厂商原始协议
```

构建后生成三个目标：

| CMake 目标 | 类型 | 用途 |
| --- | --- | --- |
| `thickness::protocol` | 库 | 仅做协议解析，不依赖 BlueZ/GLib |
| `thickness::bluez` | 库 | Linux BLE 连接层，并链接协议库 |
| `thickness_reader` | 可执行程序 | 可直接部署的终端接收程序 |

## 5. 环境要求

### 5.1 支持目标

- Linux，使用 systemd/BlueZ 系统 D-Bus。
- C++17 编译器。
- CMake 3.16 或更高版本。
- `gio-2.0` 开发库。
- 当前实测：Ubuntu 22.04 x86_64、BlueZ 5.64。
- 设计目标：Debian/Ubuntu x86_64 与 aarch64，包括 Intel NUC 和香橙派。

### 5.2 安装依赖

Debian/Ubuntu：

```bash
sudo apt update
sudo apt install bluez build-essential cmake pkg-config libglib2.0-dev
sudo systemctl enable --now bluetooth
```

确认适配器状态：

```bash
rfkill list bluetooth
bluetoothctl show
```

期望看到：

- `Soft blocked: no`
- `Hard blocked: no`
- `Powered: yes`
- `bluetooth.service` 为 active

在容器内运行时必须显式提供宿主机蓝牙控制器和 system D-Bus；普通 Docker 容器默认
通常无法直接访问它们。生产环境建议先在宿主 Linux 上部署。

## 6. 编译、测试与安装

### 6.1 标准 Release 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

生成的终端程序：

```text
build/thickness_reader
```

### 6.2 Debug 构建

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```

### 6.3 构建回调示例

```bash
cmake -S . -B build -DTHICKNESS_BUILD_EXAMPLES=ON
cmake --build build --parallel
```

额外生成 `build/thickness_callback_example`。

### 6.4 安装

安装到系统默认前缀：

```bash
sudo cmake --install build
```

安装到自定义目录：

```bash
cmake --install build --prefix "$PWD/dist"
```

安装内容包括可执行程序、两个静态库、公共头文件以及
`ThicknessReaderConfig.cmake`。

## 7. 程序运行指令

### 7.1 默认设备

```bash
./build/thickness_reader
```

默认按名称 `N26Y06M0445` 查找设备。

### 7.2 指定设备名称

```bash
./build/thickness_reader --device N26Y06M0445
```

### 7.3 指定设备 MAC

```bash
./build/thickness_reader --device 48:87:2D:7F:02:91
```

### 7.4 查看帮助

```bash
./build/thickness_reader --help
```

### 7.5 运行协议自测

```bash
./build/thickness_reader --self-test
```

正常结果：

```text
全部协议解析自测通过
```

### 7.6 停止程序

前台运行时按 `Ctrl+C`。程序会停止 `FFE1` 通知，并在连接由本程序创建时主动断开
设备，然后以状态码 0 退出。

### 7.7 输出说明

测量结果写入标准输出：

```text
[14:25:31.284] 厚度: 1459.3 μm (1.4593 mm)  材料: NFE  组: Default  CRC: OK
```

连接状态和错误写入标准错误：

```text
[蓝牙] 正在连接 N26Y06M0445 ...
[蓝牙] 已连接并订阅 FFE1，等待测量数据（Ctrl+C 退出）
[蓝牙] 连接已断开，准备自动重连
[蓝牙] 1 秒后重试
[协议] 丢弃 1 个 CRC 错误帧
```

因此可以只保存合法测量值：

```bash
./build/thickness_reader > measurements.log
```

或同时保存诊断信息：

```bash
./build/thickness_reader > measurements.log 2> reader-error.log
```

退出状态：

| 状态码 | 含义 |
| ---: | --- |
| 0 | 正常退出、帮助输出或自测通过 |
| 1 | D-Bus 初始化失败或自测失败 |
| 2 | 命令行参数错误 |

设备暂时找不到、连接失败或断线不会立即退出，程序会继续自动重试。

## 8. 公共 C++ 接口

### 8.1 数据类型

`thickness::Measurement`：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `micrometers` | `double` | 已处理符号并换算后的 μm 值 |
| `material` | `MaterialType` | `nfe`、`fe`、`empty` 或 `unknown` |
| `material_code` | `uint8_t` | 设备原始材料字节 |
| `group` | `std::string` | 去除尾部 NUL/空格后的组名 |

`thickness::ParseReport`：

| 字段 | 说明 |
| --- | --- |
| `measurements` | 本次 `feed()` 产生的所有合法实时测量 |
| `crc_errors` | 本次发现并丢弃的 CRC 错误数量 |
| `protocol_errors` | 本次发现的长度或格式错误数量 |

### 8.2 ReaderConfig

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `device` | `N26Y06M0445` | 设备名称、Alias 或 public MAC |
| `service_uuid` | 完整 `FFE0` UUID | 目标 GATT 服务 |
| `notify_characteristic_uuid` | 完整 `FFE1` UUID | 通知特征 |
| `reconnect_delays_seconds` | `{1,2,4,8,10}` | 重试退避；到达最后一项后保持 10 秒 |
| `dbus_timeout_ms` | `5000` | 一般 D-Bus 调用超时 |
| `connect_timeout_ms` | `15000` | 设备连接调用超时 |
| `services_timeout_ms` | `10000` | 等待 GATT 服务解析的最大时间 |
| `disconnect_on_exit` | `true` | 退出时断开由本实例建立的连接 |

### 8.3 ReaderCallbacks

| 回调 | 触发时机 |
| --- | --- |
| `notification(data, length)` | 每次收到原始 `FFE1` GATT 通知，在协议解析前触发 |
| `measurement(value)` | 收到 CRC 正确且格式合法的 `0x02` 测量后触发 |
| `state(state, message)` | 扫描、连接、已连接、重连或停止时触发 |
| `error(message)` | D-Bus、GATT 或协议发生可报告问题时触发 |

所有回调都在调用 `BluezReader::run()` 的线程串行执行。回调中不要执行长时间阻塞的
数据库、网络或计算任务；应复制必要数据后投递给业务线程。`notification` 的字节指针
只在当前回调期间有效。

业务回调抛出的异常会被连接层捕获，不会穿过 GLib C 回调边界。

### 8.4 最小集成示例

```cpp
#include <thickness/bluez_reader.hpp>

#include <iostream>
#include <utility>

int main() {
    thickness::ReaderConfig config;
    config.device = "N26Y06M0445";

    thickness::ReaderCallbacks callbacks;
    callbacks.measurement = [](const thickness::Measurement& value) {
        std::cout << value.micrometers << " μm, group=" << value.group << '\n';
    };
    callbacks.state = [](thickness::ReaderState, const std::string& message) {
        std::cerr << "state: " << message << '\n';
    };
    callbacks.error = [](const std::string& message) {
        std::cerr << "error: " << message << '\n';
    };

    thickness::BluezReader reader(std::move(config), std::move(callbacks));
    return reader.run();
}
```

`run()` 是阻塞调用，一个 `BluezReader` 实例只能运行一次。从信号处理或其他线程调用
`request_stop()` 请求退出，并确保运行线程结束后再析构对象。该类型不可复制和移动。

完整示例见 [examples/callback_consumer.cpp](examples/callback_consumer.cpp)。

### 8.5 只使用协议解析器

如果蓝牙数据来自其他框架、串口代理或录制文件，可以只链接
`thickness::protocol`：

```cpp
#include <thickness/protocol.hpp>

thickness::FrameParser parser;
thickness::ParseReport report = parser.feed(bytes, length);
for (const auto& value : report.measurements) {
    // value.micrometers / value.material / value.group
}
```

不要假设一次 BLE 通知恰好对应一帧。应为每条连接长期保留一个 `FrameParser`，把所有
通知按顺序送入 `feed()`。断开连接后调用 `reset()` 清除未完成帧。

### 8.6 CMake 集成

作为源码子目录：

```cmake
add_subdirectory(path/to/bluetooth-protocol)
target_link_libraries(your_service PRIVATE thickness::bluez)
```

安装后使用：

```cmake
find_package(ThicknessReader CONFIG REQUIRED)
target_link_libraries(your_service PRIVATE thickness::bluez)
```

使用非标准安装前缀时：

```bash
cmake -S your-project -B your-build \
  -DCMAKE_PREFIX_PATH=/path/to/thickness-reader/dist
```

## 9. NUC、香橙派与无人值守部署

### 9.1 平台注意事项

- NUC 通常使用 USB/PCIe 蓝牙适配器，确认 BIOS 和 `rfkill` 没有禁用蓝牙。
- 香橙派板载蓝牙可能依赖特定设备树、UART 固件或厂商内核；先确保
  `bluetoothctl show` 能看到控制器，再排查本程序。
- 如果使用 USB 蓝牙适配器，确认 `lsusb`、`dmesg` 和 `bluetooth.service` 正常。
- 代码按字节显式组合小端整数，不依赖 CPU 对齐或宿主字节序。
- 当前只在 x86_64 实机验证；aarch64 构建接口已准备好，上板后仍需完成硬件回归。

### 9.2 systemd 部署

先安装程序：

```bash
cmake --install build --prefix /usr/local
```

复制并编辑服务模板：

```bash
sudo cp deploy/thickness-reader.service.example \
  /etc/systemd/system/thickness-reader.service
sudo editor /etc/systemd/system/thickness-reader.service
```

至少修改：

- `User=CHANGE_ME` 为实际运行账号。
- `ExecStart` 为真实安装路径。
- `DEVICE` 为设备名称或 MAC。

启动并查看日志：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now thickness-reader
systemctl status thickness-reader
journalctl -u thickness-reader -f
```

程序自身负责 BLE 断线重连；systemd 的 `Restart=on-failure` 负责进程异常退出后的恢复。

## 10. 故障排查

### 10.1 看不到蓝牙适配器

```bash
rfkill list bluetooth
systemctl status bluetooth
bluetoothctl list
bluetoothctl show
```

如果被软屏蔽：

```bash
sudo rfkill unblock bluetooth
sudo systemctl restart bluetooth
```

### 10.2 找不到测厚仪

```bash
bluetoothctl scan on
bluetoothctl devices
```

确认设备开机、距离足够近，并且未被手机或另一台电脑占用。当前实机正常信号约为
`-60` 到 `-70 dBm`，但具体阈值会受天线和环境影响。

### 10.3 能看到设备但连接失败

```bash
bluetoothctl info 48:87:2D:7F:02:91
journalctl -u bluetooth --since "10 minutes ago"
```

检查 `Blocked: no`、`Connected` 和 `ServicesResolved`。如果 BlueZ 缓存异常，可以在
确认没有其他程序占用后移除本机缓存并重新扫描：

```bash
bluetoothctl remove 48:87:2D:7F:02:91
bluetoothctl scan on
```

该操作只删除本机的设备缓存，不会重置测厚仪。

### 10.4 已连接但没有测量输出

- 必须实际触发一次测量；设备没有测量时不会周期性发送厚度。
- 确认程序已经打印“已连接并订阅 FFE1”。
- 不要使用经典蓝牙串口方式读取。
- 确认其他工具没有抢占通知或设备连接。
- 可通过 `ReaderCallbacks::notification` 记录原始通知，区分“未收到数据”和“解析失败”。

### 10.5 CRC 或协议错误

- 保留完整原始十六进制通知和程序错误输出。
- 确认 Size 使用小端，并以实际 `18 00` 处理实时帧。
- 不要假设一次通知就是一帧；必须使用流式缓冲。
- 新固件若增加命令，先通过原始通知回调抓取，再扩展 `FrameParser`。

### 10.6 D-Bus 权限或容器错误

若出现“无法连接系统 D-Bus”或 `Operation not permitted`，确认程序是在宿主机运行，
运行账号能访问 system bus，且 `bluetoothd` 正常。容器部署需要额外挂载 D-Bus socket、
传递蓝牙设备和设置合适权限，不属于当前默认部署方式。

## 11. 后续开发说明

### 11.1 增加数据组 `0x03`

建议保持以下边界：

1. 在 `protocol.hpp` 增加独立的数据组结构体，不把 100 条记录混入 `Measurement`。
2. 在 `ParseReport` 增加数据组结果集合，保持现有 `measurements` 兼容。
3. 在 `FrameParser::feed()` 的命令分派中加入 `0x03`。
4. 按协议校验 16 字节组名、8 字节上下限、400 字节测量数据和 100 字节材料类型。
5. 增加完整帧、分片、粘包、CRC 错误和边界数量测试。
6. 在 `ReaderCallbacks` 增加数据组回调；不要复用实时测量回调表达不同语义。

### 11.2 增加设备写入命令

写入应放在 BlueZ 传输层，通过 `FFE1` 或 `FFE2` 的 `WriteValue` 实现，不能放进纯协议
解析库。新增接口前必须从厂商确认：目标特征、命令格式、是否需要 response、最大写入
长度、超时和重试是否允许。当前协议 PDF 没有提供这些定义，因此不要凭猜测发送数据。

### 11.3 增加存储或网络上报

不要在 `measurement` 回调中直接执行慢 I/O。推荐数据流：

```text
BluezReader 回调 → 线程安全队列 → 数据库/消息队列/HTTP/MQTT 工作线程
```

队列需要明确容量、满载丢弃策略、时间戳来源和进程退出时的刷新策略。保持
`Measurement::micrometers` 为内部基准单位，在展示或外部协议边界再转换单位。

### 11.4 增加其他平台

`thickness::protocol` 可以原样复用。新增 Windows/macOS/移动端支持时，应实现新的
传输层，把收到的字节交给 `FrameParser`，不要在各平台复制一套 CRC 和厚度算法。

### 11.5 开发约定

- 保持 C++17 和架构无关实现，不使用未对齐指针强转读取协议整数。
- 公共接口只放在 `include/thickness/`，GLib 类型不得泄漏到公共头文件。
- 保持现有 CLI 参数和输出兼容；需要破坏性修改时提高项目主版本号。
- 每次协议修改至少运行 `--self-test` 和 CTest。
- 蓝牙连接层修改需要完成实机连接、通知和断线重连回归。
- 提交中注明使用的设备固件、原始报文和测试平台。

## 12. 验证记录与相关资料

当前版本已完成：

- 使用真实 `0x02` 报文验证 `1459.3、1487.0、1471.4、1484.4 μm` 等结果。
- 多帧 CRC-8 校验通过。
- 实机连接、`FFE1` 通知、正常退出和断线自动重连验证。
- 前导噪声、拆包、粘包、CRC 错误和负数符号-幅值自测。
- 安装后由独立 CMake 工程通过 `find_package(ThicknessReader)` 链接验证。

进一步的接口、线程与部署说明见 [docs/INTEGRATION.md](docs/INTEGRATION.md)。修改协议前
应同时核对厂商 PDF、实机原始报文和本项目测试，实机行为优先于文档中的明显笔误。
