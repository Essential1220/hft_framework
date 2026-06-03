#pragma once
// ============================================
// binary_io.h - Variable-length binary I/O & fixed-point encoding (变长二进制读写 & 定点数编码)
// ============================================

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace hft {
namespace binary_io {

// Price fixed-point scale: 4 decimal places (价格定点数精度: 4位小数)
constexpr int64_t kPriceScale = 10000;
// Turnover fixed-point scale: 2 decimal places (成交额定点数精度: 2位小数)
constexpr int64_t kTurnoverScale = 100;

// ZigZag encoding: maps signed int64 to unsigned uint64 for varint (ZigZag 编码: 将有符号整数映射到无符号整数)
inline uint64_t zigzag_encode(int64_t value) {
    return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
}

// Reverse ZigZag encoding back to signed int64 (ZigZag 解码: 还原为有符号整数)
inline int64_t zigzag_decode(uint64_t value) {
    return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

// Write variable-length unsigned integer (写入变长无符号整数)
inline bool write_varuint(std::ostream& os, uint64_t value) {
    while (value >= 0x80) {
        const unsigned char byte = static_cast<unsigned char>((value & 0x7F) | 0x80);
        os.put(static_cast<char>(byte));
        value >>= 7;
    }
    os.put(static_cast<char>(value));
    return static_cast<bool>(os);
}

// Read variable-length unsigned integer (读取变长无符号整数)
inline bool read_varuint(std::istream& is, uint64_t* value_out) {
    uint64_t value = 0;
    int shift = 0;
    while (shift < 64) {
        const int ch = is.get();
        if (ch == EOF) {
            return false;
        }
        value |= static_cast<uint64_t>(ch & 0x7F) << shift;
        if ((ch & 0x80) == 0) {
            *value_out = value;
            return true;
        }
        shift += 7;
    }
    return false;
}

// Write variable-length signed integer via ZigZag (通过 ZigZag 编码写入变长有符号整数)
inline bool write_varint(std::ostream& os, int64_t value) {
    return write_varuint(os, zigzag_encode(value));
}

// Read variable-length signed integer via ZigZag (通过 ZigZag 解码读取变长有符号整数)
inline bool read_varint(std::istream& is, int64_t* value_out) {
    uint64_t encoded = 0;
    if (!read_varuint(is, &encoded)) {
        return false;
    }
    *value_out = zigzag_decode(encoded);
    return true;
}

// Write length-prefixed string (写入带长度前缀的字符串)
inline bool write_string(std::ostream& os, const std::string& value) {
    return write_varuint(os, value.size()) &&
           static_cast<bool>(os.write(value.data(), static_cast<std::streamsize>(value.size())));
}

// Read length-prefixed string (读取带长度前缀的字符串)
inline bool read_string(std::istream& is, std::string* value_out) {
    uint64_t size = 0;
    if (!read_varuint(is, &size)) {
        return false;
    }
    if (size > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
        return false;
    }
    std::string value(size, '\0');
    if (!is.read(value.data(), static_cast<std::streamsize>(size))) {
        return false;
    }
    *value_out = std::move(value);
    return true;
}

// Convert floating-point price to fixed-point integer (将浮点价格转为定点整数)
inline int64_t fixed_price(double value) {
    return static_cast<int64_t>(std::llround(value * static_cast<double>(kPriceScale)));
}

// Restore fixed-point integer back to floating-point price (将定点整数还原为浮点价格)
inline double restore_price(int64_t value) {
    return static_cast<double>(value) / static_cast<double>(kPriceScale);
}

// Convert floating-point turnover to fixed-point integer (将浮点成交额转为定点整数)
inline int64_t fixed_turnover(double value) {
    return static_cast<int64_t>(std::llround(value * static_cast<double>(kTurnoverScale)));
}

// Restore fixed-point integer back to floating-point turnover (将定点整数还原为浮点成交额)
inline double restore_turnover(int64_t value) {
    return static_cast<double>(value) / static_cast<double>(kTurnoverScale);
}

} // namespace binary_io
} // namespace hft
