#pragma once
// ============================================
// shm_queue.h - Cross-platform shared-memory SPSC queue
// Same ring-buffer semantics as SPSCQueue, but the buffer lives
// in a named shared-memory region (mmap / CreateFileMapping).
// Producer calls create(), consumer calls open(), both get
// the same push/pop API as SPSCQueue.
// ============================================

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hft {

struct ShmQueueHeader {
    char     magic[8];          // "HFTQUEUE"
    uint32_t version;           // protocol version (1)
    uint32_t element_size;      // sizeof(T)
    uint64_t capacity;          // N (power of 2)
    uint64_t producer_pid;
    uint64_t consumer_pid;
    int64_t  last_write_ns;

    alignas(64) std::atomic<size_t> head;
    alignas(64) std::atomic<size_t> tail;
    alignas(64) std::atomic<size_t> drop_count;
};

static_assert(std::is_standard_layout_v<ShmQueueHeader>,
              "ShmQueueHeader must be standard layout for cross-process use");

template <typename T, size_t N>
class ShmQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    static constexpr size_t kMask = N - 1;

public:
    ShmQueue() = default;
    ~ShmQueue() { close(); }

    ShmQueue(const ShmQueue&) = delete;
    ShmQueue& operator=(const ShmQueue&) = delete;

    bool create(const std::string& name) {
        if (mapped_) return false;
        is_creator_ = true;
        size_t total = total_size();

#ifdef _WIN32
        std::wstring wname = to_wide("Local\\hft_" + name);
        hmap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, static_cast<DWORD>(total),
                                   wname.c_str());
        if (!hmap_) return false;
        base_ = static_cast<uint8_t*>(MapViewOfFile(hmap_, FILE_MAP_ALL_ACCESS, 0, 0, total));
        if (!base_) { CloseHandle(hmap_); hmap_ = nullptr; return false; }
#else
        shm_name_ = "/hft_" + name;
        fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) return false;
        if (ftruncate(fd_, static_cast<off_t>(total)) != 0) {
            ::close(fd_); fd_ = -1; return false;
        }
        base_ = static_cast<uint8_t*>(
            mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (base_ == MAP_FAILED) { base_ = nullptr; ::close(fd_); fd_ = -1; return false; }
#endif
        mapped_ = true;
        init_header();
        return true;
    }

    bool open(const std::string& name) {
        if (mapped_) return false;
        is_creator_ = false;
        size_t total = total_size();

#ifdef _WIN32
        std::wstring wname = to_wide("Local\\hft_" + name);
        hmap_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
        if (!hmap_) return false;
        base_ = static_cast<uint8_t*>(MapViewOfFile(hmap_, FILE_MAP_ALL_ACCESS, 0, 0, total));
        if (!base_) { CloseHandle(hmap_); hmap_ = nullptr; return false; }
#else
        shm_name_ = "/hft_" + name;
        fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0666);
        if (fd_ < 0) return false;
        base_ = static_cast<uint8_t*>(
            mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (base_ == MAP_FAILED) { base_ = nullptr; ::close(fd_); fd_ = -1; return false; }
#endif
        mapped_ = true;
        auto* hdr = header();
        if (std::memcmp(hdr->magic, "HFTQUEUE", 8) != 0 ||
            hdr->version != 1 ||
            hdr->element_size != sizeof(T) ||
            hdr->capacity != N) {
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (!mapped_) return;
#ifdef _WIN32
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (hmap_) { CloseHandle(hmap_); hmap_ = nullptr; }
#else
        if (base_) { munmap(base_, total_size()); base_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (is_creator_ && !shm_name_.empty()) {
            shm_unlink(shm_name_.c_str());
            shm_name_.clear();
        }
#endif
        mapped_ = false;
    }

    bool push(const T& item) {
        if (!mapped_) return false;
        auto* hdr = header();
        size_t h = hdr->head.load(std::memory_order_relaxed);
        size_t next = (h + 1) & kMask;
        if (next == hdr->tail.load(std::memory_order_acquire)) {
            hdr->drop_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::memcpy(&buf()[h], &item, sizeof(T));
        hdr->head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        if (!mapped_) return false;
        auto* hdr = header();
        size_t t = hdr->tail.load(std::memory_order_relaxed);
        if (t == hdr->head.load(std::memory_order_acquire))
            return false;
        std::memcpy(&item, &buf()[t], sizeof(T));
        hdr->tail.store((t + 1) & kMask, std::memory_order_release);
        return true;
    }

    size_t drop_count() const {
        if (!mapped_) return 0;
        return header()->drop_count.load(std::memory_order_relaxed);
    }

    size_t capacity() const { return N - 1; }
    bool is_valid() const { return mapped_; }

private:
    static constexpr size_t header_size() {
        constexpr size_t raw = sizeof(ShmQueueHeader);
        return (raw + 63) & ~size_t(63);
    }
    static constexpr size_t total_size() {
        return header_size() + sizeof(T) * N;
    }

    ShmQueueHeader* header() const {
        return reinterpret_cast<ShmQueueHeader*>(base_);
    }
    T* buf() const {
        return reinterpret_cast<T*>(base_ + header_size());
    }

    void init_header() {
        auto* hdr = header();
        std::memset(hdr, 0, sizeof(ShmQueueHeader));
        std::memcpy(hdr->magic, "HFTQUEUE", 8);
        hdr->version = 1;
        hdr->element_size = sizeof(T);
        hdr->capacity = N;
#ifdef _WIN32
        hdr->producer_pid = static_cast<uint64_t>(GetCurrentProcessId());
#else
        hdr->producer_pid = static_cast<uint64_t>(getpid());
#endif
        new (&hdr->head) std::atomic<size_t>(0);
        new (&hdr->tail) std::atomic<size_t>(0);
        new (&hdr->drop_count) std::atomic<size_t>(0);
    }

#ifdef _WIN32
    static std::wstring to_wide(const std::string& s) {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring ws(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
        if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
        return ws;
    }
    HANDLE hmap_ = nullptr;
#else
    int fd_ = -1;
    std::string shm_name_;
#endif

    uint8_t* base_ = nullptr;
    bool mapped_ = false;
    bool is_creator_ = false;
};

} // namespace hft
