/**
 * 文件功能：仪器采集的抽象接口
 *
 * 职责说明：屏蔽真实 BLE 与文件回放的差异，统一通过事件回调交付测量。
 * 调用关系：main 调用 make_instrument；实现位于 src/instrument/instrument.cpp。
 * 阅读提示：virtual 表示通过接口调用具体实现；unique_ptr 独占对象并在离开作用域时自动销毁。
 */
#pragma once
#include "inspection/config.hpp"
#include "inspection/events.hpp"
#include "inspection/support.hpp"
#include <functional>
#include <memory>
namespace inspection {
// 模块一：事件出口。std::function 可保存 lambda；参数按值传递，返回值表示队列是否接受事件。
using EventSink = std::function<bool(Event)>;
// 仪器模块：拥有采集线程，回调只复制数据并投递；停止后 join，保证回调不再访问业务队列。
// 模块二：采集接口。虚析构允许通过基类指针安全销毁 BLE/回放实现。
class IInstrument {
  public:
    virtual ~IInstrument() = default;
    // 创建采集线程并返回；避免在主线程里持续等待蓝牙通知。
    virtual void start() = 0;
    // 请求停止并 join（等待线程结束），确保对象销毁时没有回调仍在访问它。
    virtual void stop() = 0;
    // 仿真入口仅回放后端实现；真实 BLE 不接受伪造测量。
    virtual bool test_control(const Json&) = 0;
};
// 模块三：后端工厂。根据配置生成一个具体采集对象；clock 和 sink 的寿命必须覆盖该对象。
std::unique_ptr<IInstrument> make_instrument(const Config&, const IClock&, EventSink);
} // namespace inspection
