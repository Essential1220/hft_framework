#pragma once
// ============================================
// wal_writer.h - Write-Ahead Log append writer
// Binary WAL format: [4B size][8B timestamp_ns][1B type][NB payload][4B CRC32]
// ============================================

#include "common/async_writer.h"
#include "common/crc32.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace hft {

enum class WalEntryType : uint8_t {
    OrderUpdate   = 1,
    TradeRecord   = 2,
    PositionSnap  = 3,
    Checkpoint    = 4,
};

#pragma pack(push, 1)
struct WalEntryHeader {
    uint32_t size;          // total entry size including header + payload + CRC
    int64_t  timestamp_ns;
    uint8_t  type;
};
#pragma pack(pop)

static constexpr size_t kWalHeaderSize = sizeof(WalEntryHeader);
static constexpr size_t kWalCrcSize = sizeof(uint32_t);

class WalWriter {
public:
    bool open(const std::string& path) {
        writer_ = create_async_writer();
        if (!writer_) return false;
        return writer_->open(path);
    }

    bool is_open() const { return writer_ && writer_->is_open(); }

    bool append(WalEntryType type, const void* payload, size_t payload_len) {
        if (!writer_ || !writer_->is_open()) return false;

        const uint32_t total_size = static_cast<uint32_t>(
            kWalHeaderSize + payload_len + kWalCrcSize);

        buf_.resize(total_size);

        WalEntryHeader hdr{};
        hdr.size = total_size;
        hdr.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        hdr.type = static_cast<uint8_t>(type);

        std::memcpy(buf_.data(), &hdr, kWalHeaderSize);
        if (payload_len > 0) {
            std::memcpy(buf_.data() + kWalHeaderSize, payload, payload_len);
        }

        uint32_t crc = CRC32::compute(buf_.data(), kWalHeaderSize + payload_len);
        std::memcpy(buf_.data() + kWalHeaderSize + payload_len, &crc, kWalCrcSize);

        return writer_->write(buf_.data(), total_size);
    }

    template <typename T>
    bool append(WalEntryType type, const T& obj) {
        static_assert(std::is_trivially_copyable_v<T>);
        return append(type, &obj, sizeof(T));
    }

    bool write_checkpoint() {
        return append(WalEntryType::Checkpoint, nullptr, 0);
    }

    void flush() {
        if (writer_) writer_->flush();
    }

    void close() {
        if (writer_) {
            writer_->flush();
            writer_->close();
        }
    }

private:
    std::unique_ptr<IAsyncWriter> writer_;
    std::vector<uint8_t> buf_;
};

} // namespace hft
