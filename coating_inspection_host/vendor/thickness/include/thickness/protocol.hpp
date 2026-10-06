/**
 * 文件功能：测厚仪原始字节协议接口
 *
 * 职责说明：定义材料类型、解析后的测量和流式解析器，不依赖业务或蓝牙设备句柄。
 * 调用关系：BLE 通知/回放字节 → FrameParser::feed → ParseReport → 业务适配层。
 * 阅读提示：BLE 一次通知不一定恰好是一帧；解析器负责缓存半帧、拆分多帧与校验 CRC。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace thickness {

// 模块一：材料分类。nfe 非铁磁、fe 铁磁、empty 无有效材料、unknown 未识别代码。
enum class MaterialType {
    nfe,
    fe,
    empty,
    unknown,
};

// 模块二：厂商测量值。此结构不含业务任务/候选身份，由上层适配器补充。
struct Measurement {
    // 将协议整数转换为微米（μm）用于显示，负测量值保持原符号。
    double micrometers = 0.0;
    // 本项目扩展：保留协议整数，单位为千分之一微米。
    std::int64_t milli_um = 0;
    // 归一化枚举与原始材料码同时保留；group 是设备携带的组名。
    MaterialType material = MaterialType::unknown;
    std::uint8_t material_code = 0;
    std::string group;
};

// 每次 feed 的结果：可有零个、一个或多个测量，同时报告本次丢弃帧的错误计数。
struct ParseReport {
    std::vector<Measurement> measurements;
    std::size_t crc_errors = 0;
    std::size_t protocol_errors = 0;
};

// 仪器协议流式解析器：feed 可以接收任意长度的字节块，
// 支持半帧补齐、一块多帧以及帧头之前的噪声。
// 模块三：流式解析器。对象保存跨通知缓冲区，必须由一个采集线程串行调用。
class FrameParser {
public:
    // 指针版本用于蓝牙通知，vector 版本用于回放；返回值拥有自己的测量数据。
    ParseReport feed(const std::uint8_t* data, std::size_t length);
    ParseReport feed(const std::vector<std::uint8_t>& data);
    // 连接变化时清空半帧，防止上一连接剩余数据与新连接通知拼成假帧。
    void reset();

private:
    // 只有完整帧长度和 CRC 通过后才调用，按固定偏移读取实时测量字段。
    Measurement parse_realtime() const;
    // 保存尚未消费的字节；feed 接收的临时指针不会被保留。
    std::vector<std::uint8_t> buffer_;
};

// 模块四：协议基础工具。CRC-8 用来检查传输字节损坏，材料转换用来解释设备编码。
std::uint8_t crc8(const std::uint8_t* data, std::size_t length);
MaterialType material_type(std::uint8_t code);
std::string_view material_name(MaterialType material);

}  // namespace thickness
