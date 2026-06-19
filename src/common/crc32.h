#pragma once
// ============================================
// crc32.h - CRC32 lookup table implementation
// Used by WAL for record integrity verification.
// ============================================

#include <cstddef>
#include <cstdint>

namespace hft {

class CRC32 {
public:
    static uint32_t compute(const void* data, size_t len) {
        const uint32_t* tbl = table();
        auto* buf = static_cast<const uint8_t*>(data);
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; ++i) {
            crc = tbl[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFF;
    }

    static uint32_t update(uint32_t crc, const void* data, size_t len) {
        const uint32_t* tbl = table();
        auto* buf = static_cast<const uint8_t*>(data);
        crc = ~crc;
        for (size_t i = 0; i < len; ++i) {
            crc = tbl[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        }
        return ~crc;
    }

private:
    static const uint32_t* table() {
        static uint32_t t[256] = {};
        static bool init = false;
        if (!init) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int j = 0; j < 8; ++j) {
                    c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
                }
                t[i] = c;
            }
            init = true;
        }
        return t;
    }
};

} // namespace hft
