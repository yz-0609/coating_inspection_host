/**
 * 文件功能：云发布接口与未配置实现
 *
 * 职责说明：预留提交和平台回复契约，当前使用明确返回未配置状态的适配器。
 * 调用关系：业务展示 cloud 状态；检测完成事件先保存在本地 Outbox。
 * 阅读提示：提交成功不等于平台接受；当前实现不会假装上传成功。
 */
#pragma once
#include "inspection/support.hpp"
#include <functional>
namespace inspection {
// 云适配接口：event_id 稳定，request_id 标识单次发送；成功回复必须携带两者。
// publish 返回只是提交状态，不等同平台接受。未来适配器使用回调交付接受/拒绝。
// 模块一：发送提交状态。Submitted 已提交，NotConfigured 未配置，Failed 提交失败。
enum class PublishSubmission { Submitted, NotConfigured, Failed };
// 平台回复：event_id 关联稳定业务事件，request_id 关联某一次发送尝试，error 保存失败说明。
struct CloudReply {
    std::string event_id, request_id, error;
    // 只有平台明确接受时才为 true；本地调用返回不是确认依据。
    bool platform_accepted = false;
};
// 模块二：云适配接口。未来具体平台实现负责请求和异步回复，业务保持平台无关。
class ICloudPublisher {
  public:
    virtual ~ICloudPublisher() = default;
    // 返回可展示的适配器状态；publish 的 reply 用来交付平台最终回复。
    virtual std::string status() const = 0;
    virtual PublishSubmission publish(const Json& event, const std::string& request_id,
                                      std::function<void(CloudReply)> reply) = 0;
    virtual void stop() = 0;
};
// 模块三：本期默认实现。状态恒为 NOT_CONFIGURED，publish 不发送请求。
class DisabledCloudPublisher final : public ICloudPublisher {
  public:
    std::string status() const override {
        return "NOT_CONFIGURED";
    }
    PublishSubmission publish(const Json&, const std::string&,
                              std::function<void(CloudReply)>) override;
    // 没有网络线程或请求，因此停止操作无需额外处理。
    void stop() override {
    }
};
} // namespace inspection
