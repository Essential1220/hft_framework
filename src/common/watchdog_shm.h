#pragma once
// ============================================
// watchdog_shm.h - Shared memory structure for cross-process watchdog
// Engine writes heartbeat; standalone watchdog process reads + monitors.
// ============================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hft {

struct WatchdogShm {
    char magic[8];
    std::atomic<int64_t> heartbeat_ms;
    std::atomic<int32_t> engine_pid;
    std::atomic<int32_t> risk_mode;
    std::atomic<int32_t> running;
};

static constexpr const char kWatchdogMagic[8] = {'H','F','T','W','D','O','G','1'};

class WatchdogShmHelper {
public:
    static WatchdogShm* create(const std::string& name) {
#ifdef _WIN32
        HANDLE h = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, static_cast<DWORD>(sizeof(WatchdogShm)),
            std::wstring(name.begin(), name.end()).c_str());
        if (!h) return nullptr;
        void* ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WatchdogShm));
        if (!ptr) { CloseHandle(h); return nullptr; }
        handle_ = h;
#else
        int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd < 0) return nullptr;
        ftruncate(fd, sizeof(WatchdogShm));
        void* ptr = mmap(nullptr, sizeof(WatchdogShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (ptr == MAP_FAILED) return nullptr;
#endif
        auto* shm = static_cast<WatchdogShm*>(ptr);
        std::memcpy(shm->magic, kWatchdogMagic, 8);
        shm->heartbeat_ms.store(0, std::memory_order_relaxed);
        shm->engine_pid.store(0, std::memory_order_relaxed);
        shm->risk_mode.store(0, std::memory_order_relaxed);
        shm->running.store(1, std::memory_order_release);
        return shm;
    }

    static WatchdogShm* open(const std::string& name) {
#ifdef _WIN32
        HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
            std::wstring(name.begin(), name.end()).c_str());
        if (!h) return nullptr;
        void* ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WatchdogShm));
        if (!ptr) { CloseHandle(h); return nullptr; }
        handle_ = h;
#else
        int fd = shm_open(name.c_str(), O_RDWR, 0644);
        if (fd < 0) return nullptr;
        void* ptr = mmap(nullptr, sizeof(WatchdogShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (ptr == MAP_FAILED) return nullptr;
#endif
        auto* shm = static_cast<WatchdogShm*>(ptr);
        if (std::memcmp(shm->magic, kWatchdogMagic, 8) != 0) {
            close_ptr(shm);
            return nullptr;
        }
        return shm;
    }

    static void close_ptr(WatchdogShm* shm) {
        if (!shm) return;
#ifdef _WIN32
        UnmapViewOfFile(shm);
        if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
#else
        munmap(shm, sizeof(WatchdogShm));
#endif
    }

    static void destroy(const std::string& name) {
#ifdef _WIN32
        (void)name;
#else
        shm_unlink(name.c_str());
#endif
    }

    static int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

private:
#ifdef _WIN32
    static inline HANDLE handle_ = nullptr;
#endif
};

} // namespace hft
