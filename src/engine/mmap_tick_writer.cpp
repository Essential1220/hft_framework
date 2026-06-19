// ============================================
// mmap_tick_writer.cpp - Memory-mapped tick storage implementation
// ============================================

#include "engine/mmap_tick_writer.h"
#include "common/logger.h"

#include <filesystem>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace hft {

MmapTickWriter::~MmapTickWriter() {
    close();
}

bool MmapTickWriter::open(const std::filesystem::path& path, size_t max_ticks) {
    if (mapped_) close();
    if (max_ticks == 0) return false;

    capacity_ = max_ticks;
    file_size_ = sizeof(MmapTickHeader) + max_ticks * sizeof(TickData);

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

#ifdef _WIN32
    file_handle_ = CreateFileW(
        path.wstring().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file_handle_ == INVALID_HANDLE_VALUE) {
        LOG_ERROR("mmap: CreateFile failed for " + path.string());
        return false;
    }

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(file_size_);
    if (!SetFilePointerEx(file_handle_, li, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(file_handle_)) {
        LOG_ERROR("mmap: SetEndOfFile failed");
        CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    mapping_handle_ = CreateFileMappingW(
        file_handle_, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!mapping_handle_) {
        LOG_ERROR("mmap: CreateFileMapping failed");
        CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    mapped_ = MapViewOfFile(mapping_handle_, FILE_MAP_WRITE, 0, 0, 0);
    if (!mapped_) {
        LOG_ERROR("mmap: MapViewOfFile failed");
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
        CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        LOG_ERROR("mmap: open failed for " + path.string());
        return false;
    }
    if (ftruncate(fd_, static_cast<off_t>(file_size_)) != 0) {
        LOG_ERROR("mmap: ftruncate failed");
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    mapped_ = mmap(nullptr, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped_ == MAP_FAILED) {
        LOG_ERROR("mmap: mmap failed");
        mapped_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
#endif

    auto* header = static_cast<MmapTickHeader*>(mapped_);
    *header = MmapTickHeader{};
    header->tick_size = sizeof(TickData);
    header->capacity = capacity_;
    header->count = 0;
    write_idx_ = 0;

    LOG_INFO("mmap tick writer opened: " + path.string() +
             " capacity=" + std::to_string(capacity_) +
             " file_size=" + std::to_string(file_size_));
    return true;
}

void MmapTickWriter::write(const TickData& tick) {
    if (!mapped_ || write_idx_ >= capacity_) return;

    auto* base = static_cast<char*>(mapped_) + sizeof(MmapTickHeader);
    std::memcpy(base + write_idx_ * sizeof(TickData), &tick, sizeof(TickData));
    ++write_idx_;

    auto* header = static_cast<MmapTickHeader*>(mapped_);
    header->count = write_idx_;
}

void MmapTickWriter::close() {
    if (!mapped_) return;

#ifdef _WIN32
    FlushViewOfFile(mapped_, 0);
    UnmapViewOfFile(mapped_);
    mapped_ = nullptr;
    if (mapping_handle_) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
    if (file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
    }
#else
    msync(mapped_, file_size_, MS_SYNC);
    munmap(mapped_, file_size_);
    mapped_ = nullptr;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif

    LOG_INFO("mmap tick writer closed: wrote " + std::to_string(write_idx_) + " ticks");
    write_idx_ = 0;
    capacity_ = 0;
    file_size_ = 0;
}

} // namespace hft
