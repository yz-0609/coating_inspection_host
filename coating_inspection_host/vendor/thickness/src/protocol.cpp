/**
 * 文件功能：测厚仪流式组帧、CRC 与测量解码
 *
 * 职责说明：识别 0xBB 帧头，检查长度与 CRC，把实时测量转换为整数厚度和材料。
 * 调用关系：feed 收集字节 → 找帧头 → 长度检查 → CRC → parse_realtime → 返回报告。
 * 阅读提示：仪器字节使用小端序；工位 Modbus 的高低字顺序是另一套协议，不能混用。
 */
#include "thickness/protocol.hpp"

#include <algorithm>

namespace thickness {
namespace {

// 模块一：长度约束。实时 CMD=02 的声明长度为 24，允许解析的最大声明长度为 4096。
constexpr std::size_t kRealtimeDeclaredSize = 24;
constexpr std::size_t kMaximumDeclaredSize = 4096;

}  // namespace

// 模块二：CRC-8 校验。初值 0，多项式 0x07，每字节异或后处理 8 位。
std::uint8_t crc8(const std::uint8_t* data, std::size_t length) {
    std::uint8_t crc = 0;
    while (length-- > 0) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) {
            // 最高位为 1 时左移再异或 0x07，否则只左移；转回 uint8_t 保留 8 位结果。
            crc = (crc & 0x80U) != 0U
                      ? static_cast<std::uint8_t>((crc << 1U) ^ 0x07U)
                      : static_cast<std::uint8_t>(crc << 1U);
        }
    }
    return crc;
}

// 模块三：材料码解释。00 非铁磁 NFE，01 铁磁 FE，02 空，其他编码未知。
MaterialType material_type(std::uint8_t code) {
    switch (code) {
        case 0x00:
            return MaterialType::nfe;
        case 0x01:
            return MaterialType::fe;
        case 0x02:
            return MaterialType::empty;
        default:
            return MaterialType::unknown;
    }
}

// 返回静态文字的只读视图，无须分配字符串；业务适配层随后复制为自己的 std::string。
std::string_view material_name(MaterialType material) {
    switch (material) {
        case MaterialType::nfe:
            return "NFE";
        case MaterialType::fe:
            return "FE";
        case MaterialType::empty:
            return "空";
        case MaterialType::unknown:
            return "未知";
    }
    return "未知";
}

// 模块四：容器入口。转调指针版本，保证 BLE 和回放使用相同解析流程。
ParseReport FrameParser::feed(const std::vector<std::uint8_t>& data) {
    return feed(data.data(), data.size());
}

// 模块五：流式组帧。先复制新字节到 buffer_，再尽可能消费其中完整帧。
ParseReport FrameParser::feed(const std::uint8_t* data, std::size_t length) {
    if (data != nullptr && length > 0) {
        buffer_.insert(buffer_.end(), data, data + length);
    }
    ParseReport result;

    // 循环处理多个连续帧；遇到不足的帧保留缓存，下次通知继续补齐。
    while (true) {
        // 先搜索 BB 帧头；头前噪声丢弃，无帧头则清空噪声并等待下次输入。
        const auto flag = std::find(buffer_.begin(), buffer_.end(), 0xBBU);
        if (flag == buffer_.end()) {
            buffer_.clear();
            break;
        }
        buffer_.erase(buffer_.begin(), flag);

        // 至少有 BB、CMD、SIZE 低字节、SIZE 高字节，才能知道后续帧需要多长。
        if (buffer_.size() < 4) {
            break;
        }

        // 长度字段使用小端组合；声明长度包括 CMD(1)+SIZE(2)+DATA，不含 BB 和末尾 CRC。
        const std::size_t declared_size =
            static_cast<std::size_t>(buffer_[2]) |
            (static_cast<std::size_t>(buffer_[3]) << 8U);
        // 过小/过大长度直接计协议错误；只删一个字节后重新寻帧头，以便从损坏流中恢复。
        if (declared_size < 3 || declared_size > kMaximumDeclaredSize) {
            ++result.protocol_errors;
            buffer_.erase(buffer_.begin());
            continue;
        }

        // SIZE 包括 CMD（1 字节）、SIZE 自身（2 字节）和 DATA（n 字节）。
        // 实际完整帧长度是声明长度+2（BB 和 CRC），不足时退出解析循环等待半帧后续字节。
        const std::size_t frame_size = declared_size + 2;
        if (buffer_.size() < frame_size) {
            break;
        }

        // 末字节是传输 CRC，重新计算范围从 CMD 开始，共 declared_size 个字节。
        const std::uint8_t expected_crc = buffer_[frame_size - 1];
        const std::uint8_t actual_crc = crc8(buffer_.data() + 1, declared_size);
        // 校验失败不生成测量，计错并移去当前头字节，继续重新寻找可能有效的下一帧。
        if (actual_crc != expected_crc) {
            ++result.crc_errors;
            buffer_.erase(buffer_.begin());
            continue;
        }

        // 只解析实时测量命令 02；其声明长度必须正好 24，其他 CRC 正确命令直接消费跳过。
        if (buffer_[1] == 0x02U) {
            if (declared_size == kRealtimeDeclaredSize) {
                result.measurements.push_back(parse_realtime());
            } else {
                ++result.protocol_errors;
            }
        }

        // 完整帧处理后整帧移除；后面的帧仍在缓冲区，可在本轮继续解析。
        buffer_.erase(buffer_.begin(), buffer_.begin() + frame_size);
    }

    return result;
}

// 模块六：跨连接复位。丢弃旧半帧，避免两次不同连接的字节串接。
void FrameParser::reset() {
    buffer_.clear();
}

// 模块七：实时测量字段解码。只有完整帧和 CRC 已经通过后才进入。
Measurement FrameParser::parse_realtime() const {
    // 偏移 4～7 是小端 32 位厚度；最高位为负号，低 31 位是千分之一微米的绝对值。
    const std::uint32_t raw = static_cast<std::uint32_t>(buffer_[4]) |
                              (static_cast<std::uint32_t>(buffer_[5]) << 8U) |
                              (static_cast<std::uint32_t>(buffer_[6]) << 16U) |
                              (static_cast<std::uint32_t>(buffer_[7]) << 24U);
    const bool negative = (raw & 0x80000000U) != 0U;
    const std::uint32_t magnitude = raw & 0x7FFFFFFFU;

    // 偏移 9～24 固定 16 字节组名：先截断 NUL 之后的内容，再去掉尾部填充空格。
    std::string group(reinterpret_cast<const char*>(buffer_.data() + 9), 16);
    const auto nul = group.find('\0');
    if (nul != std::string::npos) {
        group.resize(nul);
    }
    while (!group.empty() && group.back() == ' ') {
        group.pop_back();
    }

    // 保留双精度 μm 便于展示，同时保留精确整数 milli_um 供业务存储与质量比较。
    Measurement measurement;
    measurement.micrometers = static_cast<double>(magnitude) / 1000.0;
    if (negative) {
        measurement.micrometers = -measurement.micrometers;
    }
    // 先提升到 int64_t 再取负，保留原协议符号和整数精度；不依赖浮点反算。
    measurement.milli_um = negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    measurement.material_code = buffer_[8];
    measurement.material = material_type(measurement.material_code);
    measurement.group = std::move(group);
    return measurement;
}

}  // namespace thickness
