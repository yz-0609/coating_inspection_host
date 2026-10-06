/**
 * 文件功能：未配置云适配器的明确返回行为
 *
 * 职责说明：实现 DisabledCloudPublisher::publish，告诉调用方尚未接入云平台。
 * 调用关系：云接口调用 publish → 返回 NotConfigured；本地 Outbox 保留待传记录。
 * 阅读提示：此处没有网络请求，也不会调用成功回调或改变数据库上传状态。
 */
#include "inspection/cloud.hpp"
namespace inspection {
// 模块：未配置的发布入口。省略参数名表示此实现不使用事件、请求号或回调。
PublishSubmission DisabledCloudPublisher::publish(const Json&, const std::string&,
                                                  std::function<void(CloudReply)>) {
    // 无平台实现时不伪造成功回调，也不改变数据库中的尝试次数。
    return PublishSubmission::NotConfigured;
}
} // namespace inspection
