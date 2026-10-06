/**
 * 文件功能：Linux BlueZ 测厚仪读取接口
 *
 * 职责说明：声明连接配置、状态和回调，以及负责 BLE 通信的读取器。
 * 调用关系：BleInstrument 创建 BluezReader，在独立线程中 run，退出时 request_stop 并等待线程。
 * 阅读提示：BlueZ 是 Linux 蓝牙服务；本模块通过系统 D-Bus 调用其 GATT 接口。
 */
#pragma once

#include "thickness/protocol.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace thickness {

// 模块一：连接生命周期。搜索、连接、已订阅、等待重连、已停止分别通知上层。
enum class ReaderState {
    searching,
    connecting,
    connected,
    reconnecting,
    stopped,
};

// 模块二：BLE 配置。device 可为名称、别名或 MAC；UUID 定位服务和通知特征。
struct ReaderConfig {
    // 可使用设备名称、设备别名或公开的 MAC 地址进行匹配。
    std::string device = "N26Y06M0445";
    std::string service_uuid = "0000ffe0-0000-1000-8000-00805f9b34fb";
    std::string notify_characteristic_uuid =
        "0000ffe1-0000-1000-8000-00805f9b34fb";
    // 重试间隔单位为秒，连续失败逐步增大到最后一档；D-Bus/连接/服务等待参数为毫秒。
    std::array<unsigned, 5> reconnect_delays_seconds{1, 2, 4, 8, 10};
    unsigned dbus_timeout_ms = 5000;
    unsigned connect_timeout_ms = 15000;
    unsigned services_timeout_ms = 10000;
    // 退出时是否断开本读取器自己建立的连接；借用既有连接时不会主动 Disconnect。
    bool disconnect_on_exit = true;
};

// 模块三：串行回调契约。回调均运行于调用 run 的线程，应尽快复制并投递数据。
struct ReaderCallbacks {
    // 回调在调用 BluezReader::run() 的线程上串行运行。
    // notification 原始字节仅在本次回调期间有效，需要保留时必须复制。
    // 原始数据指针仅在回调期间有效；measurement 得到解析结果，state/error 通知状态与错误。
    std::function<void(const std::uint8_t* data, std::size_t length)> notification;
    std::function<void(const Measurement&)> measurement;
    std::function<void(ReaderState, const std::string&)> state;
    std::function<void(const std::string&)> error;
    // 本项目扩展：结构化错误计数，便于适配层累计，避免解析错误日志文本。
    std::function<void(std::size_t crc_errors, std::size_t protocol_errors)> diagnostic;
};

// Linux/BlueZ BLE 传输：run 持续阻塞到收到停止请求或系统 D-Bus 初始化失败。
// 类禁止复制，确保设备资源、内部实现和回调地址保持唯一。
// 模块四：读取器生命周期。不可复制/移动，避免回调中的 this 和设备资源所有权失效。
class BluezReader {
public:
    explicit BluezReader(ReaderConfig config = {}, ReaderCallbacks callbacks = {});
    ~BluezReader();

    BluezReader(const BluezReader&) = delete;
    BluezReader& operator=(const BluezReader&) = delete;
    BluezReader(BluezReader&&) = delete;
    BluezReader& operator=(BluezReader&&) = delete;

    // 阻塞运行 GLib 事件循环；每个对象只运行一次。返回 0 正常结束，非 0 表示初始化/使用错误。
    int run();
    // 可从其他线程设置原子停止标志；实际退出由 GLib 循环检查，调用者还需 join 采集线程。
    void request_stop() noexcept;

private:
    // PImpl 写法：隐藏 GIO/D-Bus 细节，使包含本头文件的业务代码不必依赖 GLib 类型。
    class Impl;
    std::atomic_bool stop_requested_{false};
    std::unique_ptr<Impl> impl_;
};

}  // namespace thickness
