/**
 * 文件功能：通过 BlueZ 系统 D-Bus 接收 BLE 通知
 *
 * 职责说明：查找仪器、连接、定位 GATT 特征、订阅测量并在断线后自动重试。
 * 调用关系：run 启动 GLib 循环 → try_connect → StartNotify → PropertiesChanged → FrameParser。
 * 阅读提示：GVariant 是带类型的 D-Bus 数据容器；得到的引用需 unref，GError 需 free。
 */
#include "thickness/bluez_reader.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace thickness {
namespace {

// 模块一：D-Bus 服务和接口名。org.bluez 提供适配器、设备、GATT 服务及特征对象。
constexpr const char* kBluezService = "org.bluez";
constexpr const char* kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kProperties = "org.freedesktop.DBus.Properties";
constexpr const char* kAdapterInterface = "org.bluez.Adapter1";
constexpr const char* kDeviceInterface = "org.bluez.Device1";
constexpr const char* kServiceInterface = "org.bluez.GattService1";
constexpr const char* kCharacteristicInterface = "org.bluez.GattCharacteristic1";

// 把设备名称、MAC 和 UUID 转小写后比较；以 unsigned char 调用 tolower 避免负字符问题。
std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// 暂存 GetManagedObjects 返回的对象路径和接口字典；interfaces 是有引用所有权的 GVariant。
struct ManagedObject {
    std::string path;
    GVariant* interfaces = nullptr;
};

// 本模块的蓝牙快照，与 inspection::Snapshot 工位快照不同；描述搜索到的适配器/设备/GATT。
struct Snapshot {
    std::string adapter_path;
    bool adapter_powered = false;
    bool adapter_discovering = false;
    std::string device_path;
    bool device_connected = false;
    bool services_resolved = false;
    std::string service_path;
    std::string characteristic_path;
};

}  // namespace

// 模块二：隐藏实现（PImpl）。全部 GLib 资源和连接状态集中在这里，由 run 所在线程处理。
class BluezReader::Impl {
public:
    // 保存读取器引用、配置及回调；构造阶段不打开 D-Bus，也不连接设备。
    Impl(BluezReader& owner, ReaderConfig config, ReaderCallbacks callbacks)
        : owner_(owner), config_(std::move(config)), callbacks_(std::move(callbacks)) {}

    // 析构做幂等清理，正常 run 返回时资源已经释放；未运行对象同样可以安全销毁。
    ~Impl() { cleanup(); }

    // 模块三：运行入口。每个实例只允许运行一次，连接系统总线后进入 GLib 事件循环。
    int run() {
        if (has_run_) {
            emit_error("BluezReader::run() 每个实例只能调用一次");
            return 2;
        }
        has_run_ = true;

        GError* error = nullptr;
        // BlueZ 在系统总线上服务；初始化失败报告 GError 并返回 1，不能继续假装在线。
        bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
        if (bus_ == nullptr) {
            report_gerror("无法连接系统 D-Bus", error);
            return 1;
        }

        // 订阅 PropertiesChanged，统一接收通知 Value 和设备 Connected 的变化。
        properties_subscription_ = g_dbus_connection_signal_subscribe(
            bus_, kBluezService, kProperties, "PropertiesChanged", nullptr, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, &Impl::properties_changed_static, this, nullptr);

        // 10 ms 后首次尝试连接；每 100 ms 检查原子停止标志，回调由 GLib 主循环串行调度。
        loop_ = g_main_loop_new(nullptr, FALSE);
        retry_source_ = g_timeout_add(10, &Impl::retry_static, this);
        stop_source_ = g_timeout_add(100, &Impl::stop_check_static, this);
        g_main_loop_run(loop_);
        cleanup();
        emit_state(ReaderState::stopped, "已停止");
        return 0;
    }

private:
    // 模块四：C 回调桥接。GLib 只接受静态函数，把 user_data 转回 Impl 再调用成员函数。
    static gboolean retry_static(gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        // 重试为一次性定时器，先清句柄，try_connect 失败后才可重新安排下一次重试。
        self->retry_source_ = 0;
        self->try_connect();
        return G_SOURCE_REMOVE;
    }

    // 停止标志未设置则保留定时器；设置后退出循环并移除此定时器。
    static gboolean stop_check_static(gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (!self->owner_.stop_requested_.load()) {
            return G_SOURCE_CONTINUE;
        }
        self->stop_source_ = 0;
        if (self->loop_ != nullptr) {
            g_main_loop_quit(self->loop_);
        }
        return G_SOURCE_REMOVE;
    }

    // C 回调边界不得泄漏 C++ 异常；捕获错误并通知上层，避免异常跨越 GLib 栈帧。
    static void properties_changed_static(GDBusConnection*, const gchar*,
                                          const gchar* object_path,
                                          const gchar*, const gchar*,
                                          GVariant* parameters,
                                          gpointer user_data) noexcept {
        try {
            static_cast<Impl*>(user_data)->properties_changed(object_path,
                                                               parameters);
        } catch (const std::exception& exception) {
            static_cast<Impl*>(user_data)->emit_error(
                std::string("处理 BlueZ 通知时发生异常: ") + exception.what());
        } catch (...) {
            static_cast<Impl*>(user_data)->emit_error(
                "处理 BlueZ 通知时发生未知异常");
        }
    }

    // 模块五：属性事件处理。D-Bus 信号参数包含接口名、变化属性字典和失效属性列表。
    void properties_changed(const char* object_path, GVariant* parameters) {
        const gchar* interface_name = nullptr;
        GVariant* changed = nullptr;
        GVariant* invalidated = nullptr;
        // &s 借用接口名字符串；@a{sv} 和 @as 取得容器引用，末尾需要分别 unref。
        g_variant_get(parameters, "(&s@a{sv}@as)", &interface_name, &changed,
                      &invalidated);

        // 只接受当前通知特征路径及 GATT 特征接口的 Value，忽略其他设备的属性变化。
        if (characteristic_path_ == object_path &&
            std::strcmp(interface_name, kCharacteristicInterface) == 0) {
            GVariant* value =
                g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
            if (value != nullptr) {
                gsize length = 0;
                const auto* bytes = static_cast<const std::uint8_t*>(
                    // ay 表示字节数组；返回指针借用 Value 的内存，unref 后不能继续使用。
                    g_variant_get_fixed_array(value, &length, sizeof(std::uint8_t)));
                if (bytes != nullptr && length > 0) {
                    // 先交付原始通知供业务记录接收时间，再解析帧并交付测量，保持回调顺序一致。
                    emit_notification(bytes, length);
                    const ParseReport parsed = parser_.feed(bytes, length);
                    // 结构化错误计数避免上层解析错误日志文本；回调异常也捕获，不影响 GLib 运行。
                    if (callbacks_.diagnostic && (parsed.crc_errors || parsed.protocol_errors)) {
                        try {
                            callbacks_.diagnostic(parsed.crc_errors, parsed.protocol_errors);
                        } catch (...) {
                            emit_error("诊断回调异常");
                        }
                    }
                    // 一次通知可能包含多帧，每个解析出的测量独立回调；半帧留在 parser_ 等待。
                    for (const auto& measurement : parsed.measurements) {
                        emit_measurement(measurement);
                    }
                    if (parsed.crc_errors > 0) {
                        std::ostringstream message;
                        message << "丢弃 " << parsed.crc_errors << " 个 CRC 错误帧";
                        emit_error(message.str());
                    }
                    if (parsed.protocol_errors > 0) {
                        std::ostringstream message;
                        message << "丢弃 " << parsed.protocol_errors
                                << " 个长度或格式错误帧";
                        emit_error(message.str());
                    }
                }
                g_variant_unref(value);
            }
        }

        // 检测当前设备的断线属性，清订阅/连接标记和半帧后报告重连状态。
        if (device_path_ == object_path &&
            std::strcmp(interface_name, kDeviceInterface) == 0) {
            gboolean connected = TRUE;
            if (g_variant_lookup(changed, "Connected", "b", &connected) &&
                !connected && connected_) {
                connected_ = false;
                notifying_ = false;
                owns_connection_ = false;
                characteristic_path_.clear();
                // 旧连接的残留半帧必须丢弃；随后按退避策略重新搜索/连接。
                parser_.reset();
                emit_state(ReaderState::reconnecting,
                           "连接已断开，准备自动重连");
                schedule_retry();
            }
        }

        g_variant_unref(changed);
        g_variant_unref(invalidated);
    }

    // 模块六：蓝牙对象扫描。一次读取 BlueZ 所管理对象树，提取设备和 GATT 路径。
    std::optional<Snapshot> snapshot() {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            bus_, kBluezService, "/", kObjectManager, "GetManagedObjects", nullptr,
            // 返回类型为对象路径→接口名→属性字典的嵌套映射；超时设置限制单次同步调用。
            G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
            static_cast<gint>(config_.dbus_timeout_ms), nullptr, &error);
        if (reply == nullptr) {
            report_gerror("无法读取 BlueZ 对象", error);
            return std::nullopt;
        }

        // 先将所有对象暂存，之后分阶段寻找适配器、设备、服务和通知特征。
        std::vector<ManagedObject> objects;
        GVariantIter* iterator = nullptr;
        g_variant_get(reply, "(a{oa{sa{sv}}})", &iterator);
        const gchar* path = nullptr;
        GVariant* interfaces = nullptr;
        while (g_variant_iter_next(iterator, "{&o@a{sa{sv}}}", &path,
                                   &interfaces)) {
            objects.push_back({path, interfaces});
        }
        // 迭代器和总回复可先释放，因为各 interfaces 用 @ 获取了独立引用，后面仍然有效。
        g_variant_iter_free(iterator);
        g_variant_unref(reply);

        Snapshot result;
        const std::string wanted = lowercase(config_.device);

        // 阶段一：找适配器并读取 Powered/Discovering；当前逻辑优先记录处于开启状态的适配器。
        for (const auto& object : objects) {
            GVariant* properties = g_variant_lookup_value(
                object.interfaces, kAdapterInterface, G_VARIANT_TYPE("a{sv}"));
            if (properties == nullptr) {
                continue;
            }
            gboolean powered = FALSE;
            gboolean discovering = FALSE;
            g_variant_lookup(properties, "Powered", "b", &powered);
            g_variant_lookup(properties, "Discovering", "b", &discovering);
            if (result.adapter_path.empty() || powered) {
                result.adapter_path = object.path;
                result.adapter_powered = powered;
                result.adapter_discovering = discovering;
            }
            g_variant_unref(properties);
        }

        for (const auto& object : objects) {
            GVariant* properties = g_variant_lookup_value(
                object.interfaces, kDeviceInterface, G_VARIANT_TYPE("a{sv}"));
            if (properties == nullptr) {
                continue;
            }
            // 阶段二：按 Address、Name 或 Alias 任一匹配设备，保留连接和服务解析状态。
            const gchar* address = "";
            const gchar* name = "";
            const gchar* alias = "";
            g_variant_lookup(properties, "Address", "&s", &address);
            g_variant_lookup(properties, "Name", "&s", &name);
            g_variant_lookup(properties, "Alias", "&s", &alias);
            if (lowercase(address) == wanted || lowercase(name) == wanted ||
                lowercase(alias) == wanted) {
                gboolean connected = FALSE;
                gboolean resolved = FALSE;
                g_variant_lookup(properties, "Connected", "b", &connected);
                g_variant_lookup(properties, "ServicesResolved", "b", &resolved);
                result.device_path = object.path;
                result.device_connected = connected;
                result.services_resolved = resolved;
                g_variant_unref(properties);
                break;
            }
            g_variant_unref(properties);
        }

        // 阶段三：只在目标设备路径下寻找服务 UUID，再检查特征 UUID 与 Service 归属。
        if (!result.device_path.empty()) {
            for (const auto& object : objects) {
                // rfind(prefix,0)==0 表示以目标设备路径开头，防止取到其他 BLE 设备的同名服务。
                if (object.path.rfind(result.device_path + '/', 0) != 0) {
                    continue;
                }
                GVariant* properties = g_variant_lookup_value(
                    object.interfaces, kServiceInterface, G_VARIANT_TYPE("a{sv}"));
                if (properties == nullptr) {
                    continue;
                }
                const gchar* uuid = "";
                g_variant_lookup(properties, "UUID", "&s", &uuid);
                if (lowercase(uuid) == lowercase(config_.service_uuid)) {
                    result.service_path = object.path;
                    g_variant_unref(properties);
                    break;
                }
                g_variant_unref(properties);
            }

            // 阶段四：特征 UUID 相同还不够，必须属于已经找到的目标服务路径。
            if (!result.service_path.empty()) {
                for (const auto& object : objects) {
                    GVariant* properties = g_variant_lookup_value(
                        object.interfaces, kCharacteristicInterface,
                        G_VARIANT_TYPE("a{sv}"));
                    if (properties == nullptr) {
                        continue;
                    }
                    const gchar* uuid = "";
                    const gchar* service = "";
                    g_variant_lookup(properties, "UUID", "&s", &uuid);
                    g_variant_lookup(properties, "Service", "&o", &service);
                    if (lowercase(uuid) ==
                            lowercase(config_.notify_characteristic_uuid) &&
                        result.service_path == service) {
                        result.characteristic_path = object.path;
                        g_variant_unref(properties);
                        break;
                    }
                    g_variant_unref(properties);
                }
            }
        }

        // 扫描结束释放每个接口字典引用，结果只保留 C++ 字符串和布尔值。
        for (auto& object : objects) {
            g_variant_unref(object.interfaces);
        }
        return result;
    }

    // 模块七：通用无参数 D-Bus 调用。Connect/Disconnect/StartNotify 等共用此逻辑。
    bool call_no_args(const std::string& path, const char* interface_name,
                      const char* method, unsigned timeout_ms, bool quiet = false) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            bus_, kBluezService, path.c_str(), interface_name, method, nullptr,
            // 预期空元组回复；成功需释放回复，失败需释放 GError。quiet 仅影响错误日志。
            G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE,
            static_cast<gint>(timeout_ms), nullptr, &error);
        if (reply != nullptr) {
            g_variant_unref(reply);
            return true;
        }
        if (!quiet) {
            std::ostringstream message;
            message << path << ' ' << method << " 失败";
            report_gerror(message.str(), error);
        } else if (error != nullptr) {
            g_error_free(error);
        }
        return false;
    }

    // 模块八：连接与订阅流程。停止或已连通时不做重复操作，失败均安排后续重试。
    void try_connect() {
        if (owner_.stop_requested_.load() || connected_) {
            return;
        }

        // 每次重新获取对象树，设备路径可能随发现/重连改变，不能依赖过期地址。
        const auto current = snapshot();
        if (!current) {
            schedule_retry();
            return;
        }
        adapter_path_ = current->adapter_path;

        // 没有适配器或适配器未开启时报告原因并重试，本模块不擅自打开系统蓝牙电源。
        if (current->adapter_path.empty()) {
            emit_error("未找到蓝牙适配器");
            schedule_retry();
            return;
        }
        if (!current->adapter_powered) {
            emit_error("蓝牙适配器未开启，请先打开系统蓝牙");
            schedule_retry();
            return;
        }

        // 找不到设备时启动发现；scanning_reported_ 限制相同搜索提示反复输出。
        if (current->device_path.empty()) {
            if (!current->adapter_discovering) {
                call_no_args(current->adapter_path, kAdapterInterface,
                             "StartDiscovery", config_.dbus_timeout_ms, true);
            }
            if (!scanning_reported_) {
                emit_state(ReaderState::searching,
                           "正在查找设备 " + config_.device + " ...");
                scanning_reported_ = true;
            }
            schedule_retry();
            return;
        }

        device_path_ = current->device_path;
        scanning_reported_ = false;
        // 区分主动创建连接与借用已有连接，退出时只断开本读取器拥有的连接。
        const bool was_connected = current->device_connected;
        if (!was_connected) {
            emit_state(ReaderState::connecting,
                       "正在连接 " + config_.device + " ...");
            if (!call_no_args(device_path_, kDeviceInterface, "Connect",
                              config_.connect_timeout_ms)) {
                schedule_retry();
                return;
            }
            owns_connection_ = true;
        } else {
            owns_connection_ = false;
        }

        // 设备连接成功不等于 GATT 已可用，继续等待 ServicesResolved 和通知特征出现。
        std::optional<Snapshot> ready;
        // 按 250 ms 间隔估算尝试次数；每次 snapshot 的同步 D-Bus 耗时也会增加实际等待时长。
        const unsigned poll_count =
            std::max(1U, (config_.services_timeout_ms + 249U) / 250U);
        for (unsigned attempt = 0;
             attempt < poll_count && !owner_.stop_requested_.load(); ++attempt) {
            ready = snapshot();
            if (ready && ready->device_connected && ready->services_resolved &&
                !ready->characteristic_path.empty()) {
                break;
            }
            g_usleep(250000);
        }

        // 服务或特征未就绪时放弃本次尝试，断开自己建立的连接后退避重试。
        if (!ready || !ready->device_connected || !ready->services_resolved ||
            ready->characteristic_path.empty()) {
            emit_error("GATT 服务解析超时，稍后重试");
            disconnect_owned_device();
            schedule_retry();
            return;
        }

        // 定位成功后调用 StartNotify；只有订阅成功才标 connected，并让上层仪器在线。
        characteristic_path_ = ready->characteristic_path;
        if (!call_no_args(characteristic_path_, kCharacteristicInterface,
                          "StartNotify", config_.dbus_timeout_ms)) {
            characteristic_path_.clear();
            disconnect_owned_device();
            schedule_retry();
            return;
        }

        // 成功后重置重试档位并尝试停止发现，避免持续扫描；通知将在 PropertiesChanged 中到达。
        notifying_ = true;
        connected_ = true;
        retry_count_ = 0;
        if (!adapter_path_.empty()) {
            call_no_args(adapter_path_, kAdapterInterface, "StopDiscovery",
                         config_.dbus_timeout_ms, true);
        }
        emit_state(ReaderState::connected,
                   "已连接并订阅 FFE1，等待测量数据（Ctrl+C 退出）");
    }

    // 模块九：连接所有权管理。借用其他应用已有的连接时不会主动 Disconnect。
    void disconnect_owned_device() {
        if (owns_connection_ && !device_path_.empty()) {
            call_no_args(device_path_, kDeviceInterface, "Disconnect",
                         config_.dbus_timeout_ms, true);
        }
        owns_connection_ = false;
    }

    // 模块十：退避重试。已有定时器或停止请求时不重复安排，避免并发连接尝试。
    void schedule_retry() {
        if (retry_source_ != 0 || owner_.stop_requested_.load()) {
            return;
        }
        // 连续失败按配置档位增大延迟，到末档后保持；连接成功把 retry_count_ 清零。
        const unsigned index = std::min<unsigned>(
            retry_count_, config_.reconnect_delays_seconds.size() - 1);
        const unsigned delay = config_.reconnect_delays_seconds[index];
        retry_count_ = std::min<unsigned>(
            retry_count_ + 1, config_.reconnect_delays_seconds.size() - 1);
        emit_state(ReaderState::reconnecting,
                   std::to_string(delay) + " 秒后重试");
        retry_source_ =
            g_timeout_add_seconds(delay, &Impl::retry_static, this);
    }

    // 模块十一：安全回调出口。先检查是否注册，调用异常转为 error 回调，不能穿透 GLib。
    void emit_measurement(const Measurement& measurement) noexcept {
        if (!callbacks_.measurement) {
            return;
        }
        try {
            callbacks_.measurement(measurement);
        } catch (const std::exception& exception) {
            emit_error(std::string("测量回调发生异常: ") + exception.what());
        } catch (...) {
            emit_error("测量回调发生未知异常");
        }
    }

    // 原始通知数据只在此次调用内有效；上层需要跨线程使用时必须立即复制。
    void emit_notification(const std::uint8_t* data,
                           std::size_t length) noexcept {
        if (!callbacks_.notification) {
            return;
        }
        try {
            callbacks_.notification(data, length);
        } catch (const std::exception& exception) {
            emit_error(std::string("原始通知回调发生异常: ") + exception.what());
        } catch (...) {
            emit_error("原始通知回调发生未知异常");
        }
    }

    // 状态回调串行运行；上层通常把状态转为业务事件，避免在这里阻塞或执行数据库操作。
    void emit_state(ReaderState state, const std::string& message) noexcept {
        if (!callbacks_.state) {
            return;
        }
        try {
            callbacks_.state(state, message);
        } catch (const std::exception& exception) {
            emit_error(std::string("状态回调发生异常: ") + exception.what());
        } catch (...) {
            emit_error("状态回调发生未知异常");
        }
    }

    // 错误回调自身也可能抛异常，此处吞掉避免递归报告或越过 C 回调边界。
    void emit_error(const std::string& message) noexcept {
        if (!callbacks_.error) {
            return;
        }
        try {
            callbacks_.error(message);
        } catch (...) {
            // 应用回调的异常不能向外传播穿过 GLib 的 C 调用栈。
        }
    }

    // 把 GLib 错误复制为 C++ 字符串再释放 GError，避免错误对象泄漏。
    void report_gerror(const std::string& context, GError* error) noexcept {
        std::string message = context;
        if (error != nullptr) {
            message += ": ";
            message += error->message;
            g_error_free(error);
        }
        emit_error(message);
    }

    // 模块十二：幂等清理。先撤定时器，再停止通知/自有连接，取消信号，最后释放循环和总线。
    void cleanup() {
        // run 正常结束与析构都可调用 cleanup，只允许第一次真正释放资源。
        if (cleaned_up_) {
            return;
        }
        cleaned_up_ = true;

        // 删除定时源，保证对象销毁后 GLib 不会再用旧 this 执行回调。
        if (retry_source_ != 0) {
            g_source_remove(retry_source_);
            retry_source_ = 0;
        }
        if (stop_source_ != 0) {
            g_source_remove(stop_source_);
            stop_source_ = 0;
        }
        // 通知确已启动才 StopNotify；尚未成功订阅时无需停止。
        if (bus_ != nullptr && notifying_ && !characteristic_path_.empty()) {
            call_no_args(characteristic_path_, kCharacteristicInterface,
                         "StopNotify", config_.dbus_timeout_ms, true);
        }
        if (bus_ != nullptr && config_.disconnect_on_exit) {
            disconnect_owned_device();
        }
        // 先取消 D-Bus 信号订阅，再销毁循环和总线引用，结束所有本对象的通知入口。
        if (bus_ != nullptr && properties_subscription_ != 0) {
            g_dbus_connection_signal_unsubscribe(bus_, properties_subscription_);
            properties_subscription_ = 0;
        }
        if (loop_ != nullptr) {
            g_main_loop_unref(loop_);
            loop_ = nullptr;
        }
        if (bus_ != nullptr) {
            g_object_unref(bus_);
            bus_ = nullptr;
        }
    }

    // 模块十三：实现状态。owner_ 提供跨线程原子停止标志，其余状态由 run 线程独占。
    BluezReader& owner_;
    ReaderConfig config_;
    ReaderCallbacks callbacks_;
    GDBusConnection* bus_ = nullptr;
    GMainLoop* loop_ = nullptr;
    // GLib 分配的订阅/定时器 ID；0 表示未注册或已移除。
    guint properties_subscription_ = 0;
    guint retry_source_ = 0;
    guint stop_source_ = 0;
    std::string adapter_path_;
    std::string device_path_;
    std::string characteristic_path_;
    // 跨 BLE 通知保存半帧；断线后 reset；不与回放后端共用同一个实例。
    FrameParser parser_;
    unsigned retry_count_ = 0;
    bool has_run_ = false;
    bool scanning_reported_ = false;
    bool connected_ = false;
    bool notifying_ = false;
    // 连接所有权和 cleaned_up_ 保障资源只按本对象责任释放一次。
    bool owns_connection_ = false;
    bool cleaned_up_ = false;
};

// 模块十四：公开接口转发。make_unique 构造隐藏实现，移动配置/回调减少拷贝。
BluezReader::BluezReader(ReaderConfig config, ReaderCallbacks callbacks)
    : impl_(std::make_unique<Impl>(*this, std::move(config), std::move(callbacks))) {}

// unique_ptr 自动销毁 Impl，触发资源清理；调用者应先停线程再析构读取器。
BluezReader::~BluezReader() = default;

// 公开 run 委托 Impl；返回码供 BleInstrument 判断初始化是否成功。
int BluezReader::run() {
    return impl_->run();
}

// 只设置原子标志，不跨线程直接操作 GLib 句柄；停止检查定时器将退出事件循环。
void BluezReader::request_stop() noexcept {
    stop_requested_.store(true);
}

}  // namespace thickness
