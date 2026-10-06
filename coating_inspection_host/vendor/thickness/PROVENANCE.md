# BLE 依赖来源与本项目改动

来源：https://github.com/yz-0609/Thickness_bluetooth_protocol
固定提交：7db6610bd7cdd23348475f12dbd8ee7846105f4f

保留include/src及上游README。构建目标在本工程定义，不修改工作区外的原仓库。
上游接口新增Measurement::milli_um（int64_t），从已解码的符号-幅值直接得到千分之一微米整数；
原double micrometers接口保持兼容，厂商CRC和大小解释保持原语义。
ReaderCallbacks新增diagnostic结构化计数回调；适配层累计CRC/格式错误，不从日志字符串反解析。

本项目InstrumentEvents在适配层补充接收时间、连接代次、本地序号、候选编号。
BluezReader已有断连reset协议半帧，回放后端在连接变化时也执行reset。
上游已验证样例用于解析回归，不意味着本项目进行了新的实机测量。

上游代码仍保留原注释，新增适配模块提供中文说明。后续更换或修复依赖需更新此文件并执行解析回归。
