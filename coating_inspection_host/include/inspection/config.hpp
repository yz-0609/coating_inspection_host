/**
 * 文件功能：启动配置的数据结构与加载入口
 *
 * 职责说明：集中保存设备、时间限制、存储、测试开关和产品质量规则。
 * 调用关系：main 调用 load_config；各模块读取 Config 中属于自己的参数。
 * 阅读提示：此处默认值只是缺省配置，合法范围和跨字段约束见 src/support/config.cpp。
 */
#pragma once
#include "inspection/types.hpp"
#include <map>
#include <string>
namespace inspection {
// 配置在启动时整体校验并保持不可变；产品规则在创建任务时复制为快照。
// 模块一：启动参数集合。字符串选择后端和路径，时间单位统一为毫秒（ms）。
struct Config {
    // 工位身份用于结果追溯；instrument_type 选择 replay/ble，device_name 用于识别仪器。
    std::string station_id = "station01", instrument_type = "replay", device_name = "N26Y06M0445";
    // 回放文件与串口路径对应不同后端；database_path 决定 SQLite 数据文件的位置。
    std::string replay_file, workstation_type = "mock", serial_device,
                             database_path = "data/inspection.db";
    // 当前只允许 disabled；rules 以产品型号为键，查找对应点数、材料和厚度阈值。
    std::string cloud_adapter = "disabled";
    std::map<std::string, Rule> rules;
    // 串口波特率/从站地址；poll_ms 轮询工位、heartbeat_ms 发心跳、health_ms 判断业务健康期限。
    int baudrate = 115200, slave_id = 1, poll_ms = 100, heartbeat_ms = 200, health_ms = 500;
    // response_ms 限制单次通信；command_ms 限制业务 ACK 总等待；retries 是首次发送之外的重试次数。
    int response_ms = 150, command_ms = 1000, retries = 2, measurement_ms = 30000;
    // 数据库锁等待期限、磁盘最低余量（MiB）和默认回放间隔，避免无限阻塞。
    int database_busy_ms = 100, disk_min_mb = 64, replay_interval_ms = 1000;
    // 事件队列最大条数；满载触发故障，避免内存持续增长或悄悄漏掉关键事件。
    std::size_t queue_capacity = 128;
    // 回放循环开关、测试控制开关和原始通知日志开关；生产配置应限制测试控制。
    bool replay_loop = true, allow_test_controls = false, raw_notifications = false;
};
// 模块二：加载入口。读取 YAML 后统一校验，错误会抛异常，成功返回完整配置副本。
Config load_config(const std::string& path);
} // namespace inspection
