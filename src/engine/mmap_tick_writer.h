#pragma once
// ============================================
// mmap_tick_writer.h - Memory-mapped tick storage (内存映射 tick 存储)
// Zero-copy write path: pre-allocate a file of max_ticks * sizeof(TickData),
// map it into memory, and write ticks via memcpy — no syscall per tick.
// (零拷贝写路径: 预分配 max_ticks * sizeof(TickData) 的文件,
//  映射到内存后 memcpy 写入, 每 tick 零系统调用)
// ============================================

#include "common/types.h"

#include <cstddef>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hft {

struct MmapTickHeader {
    char magic[8] = {'H','F','T','M','M','A','P','1'};
    uint64_t tick_size = 0;
    uint64_t capacity = 0;
    uint64_t count = 0;
};

class MmapTickWriter {
public:
    MmapTickWriter() = default;
    ~MmapTickWriter();

    MmapTickWriter(const MmapTickWriter&) = delete;
    MmapTickWriter& operator=(const MmapTickWriter&) = delete;

    bool open(const std::filesystem::path& path, size_t max_ticks);
    void write(const TickData& tick);
    void close();
    size_t count() const { return write_idx_; }
    bool is_open() const { return mapped_ != nullptr; }

private:
    void* mapped_ = nullptr;
    size_t file_size_ = 0;
    size_t capacity_ = 0;
    size_t write_idx_ = 0;

#ifdef _WIN32
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace hft
