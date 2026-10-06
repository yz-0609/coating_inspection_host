/**
 * 文件功能：工位通信的抽象接口
 *
 * 职责说明：提供启动、停止、异步命令提交和测试控制入口。
 * 调用关系：业务通过 submit 提交命令；工位线程操作 Modbus 或 Mock，再回传事件。
 * 阅读提示：submit 返回 true 只表示排队成功；现场执行是否成功要等待 CommandDone。
 */
#pragma once
#include "inspection/instrument.hpp"
#include <memory>
namespace inspection {
// 工位模块：独占串口和命令确认状态。submit() 非阻塞，业务线程不能等待 RTU。
// 模块一：统一工位接口。真实 Modbus 和内存 Mock 都实现此类。
class IWorkstation {
  public:
    virtual ~IWorkstation() = default;
    // 启动设备通信线程；设备句柄只由该线程操作。
    virtual void start() = 0;
    // 停止通信并等待线程退出，释放串口资源。
    virtual void stop() = 0;
    // 按值提交一条命令，不能阻塞业务线程等待串口；false 表示无法入队。
    virtual bool submit(Command) = 0;
    // 测试控制只由 Mock 接受；真实串口的模拟器使用独立控制通道。
    virtual bool test_control(const Json&) = 0;
};
// 模块二：工厂入口。health 只供工位读取，用于判断能否继续刷新应用心跳。
std::unique_ptr<IWorkstation> make_workstation(const Config&, const IClock&, BusinessHealth&,
                                               EventSink);
} // namespace inspection
