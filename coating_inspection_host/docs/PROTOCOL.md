# 工位协议对接说明

协议来源：项目整体计划第9节，PROTOCOL_VERSION=0x0200。
Linux主站支持FC04/03/06/10所需基础能力；运行流程使用04快照、06心跳和10完整命令。
串口115200、8E1、地址1；多寄存器32位值高字在低地址。

| 输入地址 | 含义 |
|---|---|
|0000|MCU_STATE：0 BOOT、1 IDLE、2 READY、3 INSPECTING、4 COMPLETED、5 FAULT|
|0001|输入标志，bit0到位、bit1 ACTION、bit2 RESET|
|0002|输出标志，bit0 PASS、bit1 FAIL、bit2 FAULT|
|0003～0005|故障码、健康掩码、MCU心跳|
|0006～0009|uptime、活动token|
|000A～000C|ACK序号、ACK状态|
|000D～000F|输入事件序号、事件类型1 ACTION/2 RESET|
|0010～0013|复位原因、通信错误计数、协议版本|
|0014～0015|ACK_HOST_EPOCH|

一次FC04读取22个寄存器，必须返回一致快照。Linux不会从按钮瞬时电平推断按键事件。
健康掩码目前按非零判为具备健康证据，MCU必须在允许START/RESET时再次验证所有关键任务健康；
各bit定义需要双方在真实接入前进一步对齐。具体 bit 分配未在原计划中冻结，Linux不擅自添加另一套定义。

FC06只写0100变化的16位心跳。工位线程仅在业务健康戳不超过500ms时推进心跳，周期200ms。
相同值不刷新MCU健康期限。模拟器超时1500ms进入FAULT，黄灯逻辑位打开，PASS撤销。

FC10一次写0110～0118：epoch、token、seq、opcode、argument，共9个寄存器。
命令1～6分别是SYNC_SAFE、START、APPLY_RESULT、ACK_INPUT_EVENT、RESET_FAULT、ABORT。
APPLY_RESULT参数1 PASS/2 FAIL；ACK_INPUT_EVENT参数为保持中的按钮事件序号。
SYNC_SAFE/RESET_FAULT使用token0；START、APPLY_RESULT、ABORT携带任务token。

只有写响应不代表成功。ACK必须匹配本周期epoch和命令序号，ACK_STATUS必须为1，
随后还要核对当前状态、token、到位和输出。旧ACK或故障后的原幂等ACK不能再次打开PASS。
未知协议版本触发系统故障并停止变化心跳，不使用未知协议持续维持旧输出。

Linux同时只等待一个业务确认；相同内容重传不分配新序号。默认确认期限1000ms，最多2次重传。
串口错误可能发生在命令已执行之后，记录UNKNOWN并重新同步；不把未知执行强行标成失败未执行。
MCU收到非法token/epoch/状态/参数应按计划返回ACK_STATUS2～6，不通过零散寄存器打开输出。

主站事务结束后预留至少1750μs间隔。libmodbus按预期字节数处理RTU响应；
这不替代MCU接收定界实现或物理总线波形测试。实际MCU需完成t1.5/t3.5、USART TC与DE实测。
模拟器的逻辑黄灯、心跳撤销时间不能作为GPIO时间指标。
