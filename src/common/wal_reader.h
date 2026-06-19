#pragma once
// ============================================
// wal_reader.h - Write-Ahead Log replay reader
// Reads binary WAL file, verifies CRC, and yields entries.
// ============================================

#include "common/crc32.h"
#include "common/wal_writer.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace hft {

struct WalEntry {
    WalEntryType type;
    int64_t timestamp_ns;
    std::vector<uint8_t> payload;
};

class WalReader {
public:
    using EntryCallback = std::function<void(const WalEntry&)>;

    enum class ReplayResult {
        Ok,
        FileNotFound,
        ReadError,
        Truncated,
    };

    static ReplayResult replay(const std::string& path, const EntryCallback& cb,
                               size_t* entries_read = nullptr,
                               size_t* entries_corrupted = nullptr) {
        size_t read_count = 0;
        size_t corrupt_count = 0;
        if (entries_read) *entries_read = 0;
        if (entries_corrupted) *entries_corrupted = 0;

        FILE* fp = nullptr;
#ifdef _WIN32
        fopen_s(&fp, path.c_str(), "rb");
#else
        fp = std::fopen(path.c_str(), "rb");
#endif
        if (!fp) return ReplayResult::FileNotFound;

        std::vector<uint8_t> buf;
        WalEntryHeader hdr{};
        bool truncated = false;

        while (true) {
            size_t n = std::fread(&hdr, 1, kWalHeaderSize, fp);
            if (n == 0) break;
            if (n < kWalHeaderSize) { truncated = true; break; }

            if (hdr.size < kWalHeaderSize + kWalCrcSize) {
                ++corrupt_count;
                break;
            }

            const size_t remaining = hdr.size - kWalHeaderSize;
            buf.resize(remaining);
            n = std::fread(buf.data(), 1, remaining, fp);
            if (n < remaining) { truncated = true; break; }

            const size_t payload_len = remaining - kWalCrcSize;

            // Reconstruct full entry for CRC check
            uint32_t expected_crc;
            std::memcpy(&expected_crc, buf.data() + payload_len, kWalCrcSize);

            // CRC over header + payload
            uint32_t actual_crc = CRC32::compute(&hdr, kWalHeaderSize);
            if (payload_len > 0) {
                actual_crc = CRC32::update(actual_crc, buf.data(), payload_len);
            }

            if (actual_crc != expected_crc) {
                ++corrupt_count;
                continue;
            }

            WalEntry entry;
            entry.type = static_cast<WalEntryType>(hdr.type);
            entry.timestamp_ns = hdr.timestamp_ns;
            if (payload_len > 0) {
                entry.payload.assign(buf.data(), buf.data() + payload_len);
            }

            if (cb) cb(entry);
            ++read_count;
        }

        std::fclose(fp);

        if (entries_read) *entries_read = read_count;
        if (entries_corrupted) *entries_corrupted = corrupt_count;

        if (truncated) return ReplayResult::Truncated;
        return ReplayResult::Ok;
    }

    template <typename T>
    static bool extract(const WalEntry& entry, T& out) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (entry.payload.size() < sizeof(T)) return false;
        std::memcpy(&out, entry.payload.data(), sizeof(T));
        return true;
    }
};

} // namespace hft
