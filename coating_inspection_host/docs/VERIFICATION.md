# 软件验收记录

记录日期：2026-10-03。以下为本次实际执行结果，均在 Ubuntu PC 上完成，不包含真实仪器、RS485 或 STM32 实机验证。

## 构建环境与版本

- GCC 11.4.0，CMake 3.22.1，C++17。
- GLib/GIO 2.72.4、SQLite 3.37.2、yaml-cpp 0.7.0。
- libmodbus 3.1.6、nlohmann-json 3.10.5 已正式安装为 Ubuntu 系统开发包；早期验证使用同版本软件包本地解包。
- 上游 BLE 固定提交及修改见 `vendor/thickness/PROVENANCE.md`。
- Debug 构建启用 Wall/Wextra/Wpedantic/Wconversion/Wshadow，最终构建无编译警告。

## 已通过

| 检查 | 结果与范围 |
|---|---|
|普通 CTest|3/3 测试入口通过；单元测试、模拟器协议契约、31个进程集成场景|
|系统依赖标准构建|正式安装开发包后，使用全新build-system目录且不指定临时依赖路径；构建成功，CTest 3/3通过，总耗时31.13秒；服务链接系统libmodbus|
|ASan/UBSan|最终3/3入口通过，总耗时32.22秒；自研服务和上游BLE库均启用地址/未定义行为检查，未报告错误|
|自动PASS演示|真实主站＋PTY模拟工位＋回放仪器；5个确认点、PASS、1条PENDING Outbox|
|自动FAIL演示|第5点超界；5个确认点、FAIL、1条PENDING Outbox|
|自动中止演示|第2点后取消；ABORTED、保留2个确认点、没有完成事件|
|60秒连续负载|实际60.069秒，20个任务：7 PASS、7 FAIL、6中止；确认数据库完整性、结果与Outbox计数一致，无异常|
|Python语法|模拟器、工具和集成测试均通过py_compile|

60秒负载RSS从8724kB到8804kB，只作为短测样本，不用于推断24小时或长期趋势。
短测原始报告位于 `reports/soak-smoke-60s/report.json`。
演示摘要位于 `reports/demo-pass`、`reports/demo-fail`、`reports/demo-aborted`。

单元测试覆盖规则上下界、材料约束、命令高低字、ACK匹配、已验证上游仪器样例、分片/连续帧/CRC错误/负值、连接半帧清理、队列溢出、可控时钟、事务回滚、只读连接、恢复和禁用云接口。

31个进程场景包括：Mock和真实主站下的阈值/材料判定、候选绑定与同值多点、按钮事件、START拒绝、命令相同序号重传、延迟ACK、旧epoch ACK、未知协议版本、业务停滞/杀进程心跳保护、数据库写锁、真实文件写入容量限制、SIGTERM中止、OneNET未配置保护、BLE断连、工件移除、MCU复位、队列溢出、测量超时、串口断开及事务前后崩溃恢复。

故意 `_exit` 和 SIGKILL 的场景用于模拟崩溃，不执行正常析构或退出时泄漏扫描；正常退出路径已接受ASan/UBSan检查。
文件容量实验使用进程级RLIMIT_FSIZE，使SQLite实际文件写入返回错误；这不是整块磁盘被填满或真实硬件断电实验。

## 24小时长稳：运行中

已启动独立后台进程，默认每30秒一个任务，按PASS/FAIL/中止循环，使用同一服务与模拟器持续运行。
实际状态保存在：

- `reports/soak-24h/report.json`：请求时长、实际时长、完成标志、任务计数、资源样本、错误和二进制SHA256。
- `reports/soak-24h/run.log`：结束摘要或异常堆栈。
- `reports/soak-24h/pid.json`：长稳控制进程PID，方便查询或停止。

只有该报告 `completed=true`、`requested_seconds=86400` 且 `errors=[]`，才可标记24小时仿真通过。
当前文档不声称已完成24小时。需要停止时给pid.json中的PID发送SIGTERM，脚本会停止两个子进程并写入未完成报告。

## 保留为待验证

- 真实仪器一次物理测量与通知的关系、同值重复、断电重连旧值行为。
- 真实RS485电平、参考地、终端/偏置、8E1、MCU定界、DE/USART TC波形。
- 真实GPIO输出撤销时间、MCU任务健康、IWDG及复位时的物理默认态。
- 整机断电后存储一致性；WAL/FULL配置和进程崩溃测试不替代真实断电证据。
- OneNET认证、物模型、TLS、请求回复、上传/补传与平台接受确认。
- ARM依赖、构建及实机回归。

这些项目与软件/协议仿真结果分别报告，不将逻辑灯位或合成测量写成实机成果。
