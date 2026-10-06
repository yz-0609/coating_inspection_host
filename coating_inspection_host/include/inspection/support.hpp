/**
 * 文件功能：公共时钟、日志、线程队列与健康信息
 *
 * 职责说明：提供业务和设备模块共同使用的基础设施。
 * 调用关系：SystemClock 用于运行；ManualClock 用于测试；BoundedQueue 连接多个生产者与业务消费者。
 * 阅读提示：模板函数必须在头文件中提供实现；mutex 保护复合操作，atomic 用于独立共享标志。
 */
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
namespace inspection {
// 模块一：JSON 类型别名与时钟接口。Json 简写第三方 nlohmann::json 类型。
using Json = nlohmann::json;
// 时钟接口用于确定性测试；生产采用 steady_clock 判断窗口和期限。
class IClock {
  public:
    virtual ~IClock() = default;
    // 单调时间只用于时间差/超时判断，不受系统校时跳变影响。
    virtual std::int64_t mono_ms() const = 0;
    // 日历时间为 Unix 毫秒，用于日志与持久化追溯，不能用来判断采样窗口。
    virtual std::int64_t wall_ms() const = 0;
};
// 真实运行的时钟实现，具体时间获取方式见 runtime.cpp。
class SystemClock final : public IClock {
  public:
    std::int64_t mono_ms() const override;
    std::int64_t wall_ms() const override;
};
// 测试用时钟：直接设置 monotonic 和 wall，模拟时间推进或日历时间跳变。
class ManualClock final : public IClock {
  public:
    std::int64_t monotonic = 0, wall = 0;
    std::int64_t mono_ms() const override {
        return monotonic;
    }
    std::int64_t wall_ms() const override {
        return wall;
    }
};
// 模块二：唯一编号、时间质量与日志入口。uuid 用于任务、候选和业务事件身份。
std::string uuid();
std::string time_quality();
// 日志同时用于 CLI 展示和可重复测试，使用一行 JSON；中文 message 面向操作者。
// 唯一写入入口持有互斥量，保证 candidate 展示和其他线程日志不会交错。
void log(const std::string& event, const std::string& message, Json fields = Json::object());

// 有界队列：满载置独立 overflow 标志，而不是试图向满队列投递故障事件。
// close() 唤醒等待者；队列关闭后拒绝新事件，已有事件仍可读取。
// 模块三：线程安全有界 FIFO 队列。T 是事件类型；push/pop 在锁内修改队列。
template <class T> class BoundedQueue {
  public:
    // 构造时固定容量；配置校验保证生产队列容量有效。
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    }
    // 生产者入队：按值接收便于复制或移动；满载/关闭时返回 false。
    bool push(T value) {
        // RAII 锁：进入作用域加锁，return 或异常离开时自动解锁。
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return false;
        if (items_.size() >= capacity_) {
            // 队列已满时另设溢出标志并唤醒消费者，无需占用队列空间发送故障事件。
            overflow_.store(true);
            wake_.notify_all();
            return false;
        }
        // 移动对象到队尾，减少字符串/JSON 等大对象的拷贝，然后唤醒一名消费者。
        items_.push_back(std::move(value));
        wake_.notify_one();
        return true;
    }
    // 消费者出队：最多等待 wait；返回 true 才表示 result 得到一个新元素。
    bool pop(T& result, std::chrono::milliseconds wait) {
        // 条件变量等待时会暂时释放锁，醒来后重新持有锁；谓词防止虚假唤醒。
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, wait,
                       [this] { return closed_ || !items_.empty() || overflow_.load(); });
        // 超时、关闭或仅有溢出标志时可能没有元素，此时返回 false；关闭后仍可读完剩余元素。
        if (items_.empty())
            return false;
        result = std::move(items_.front());
        items_.pop_front();
        return true;
    }
    // 原子 exchange 同时读取并清除标志，让业务线程消费一次溢出通知。
    bool take_overflow() {
        return overflow_.exchange(false);
    }
    // 停止接受新元素并唤醒全部等待者，避免线程永远阻塞在队列上。
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        wake_.notify_all();
    }

  private:
    // 队列与 closed_ 由 mutex_ 保护；overflow_ 独立原子化，业务线程可直接取走。
    std::size_t capacity_;
    std::deque<T> items_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool closed_ = false;
    std::atomic_bool overflow_{false};
};
// 健康戳只有业务线程完成正常循环才推进；工位线程只读取它，不可代为更新。
// 模块四：业务健康证据。初始 -1 表示尚未完成任何正常循环，enabled=false 禁止发送心跳。
struct BusinessHealth {
    std::atomic<std::int64_t> last_loop_ms{-1};
    std::atomic_bool enabled{true};
};
} // namespace inspection
