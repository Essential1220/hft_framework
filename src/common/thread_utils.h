#pragma once
// ============================================
// thread_utils.h - Cross-platform thread affinity (跨平台线程绑核)
// ============================================

#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace hft {

inline bool set_thread_affinity(std::thread& t, int core_id) {
    if (core_id < 0) return true;
#ifdef _WIN32
    DWORD_PTR mask = 1ULL << core_id;
    return SetThreadAffinityMask(t.native_handle(), mask) != 0;
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset) == 0;
#endif
}

inline bool set_current_thread_affinity(int core_id) {
    if (core_id < 0) return true;
#ifdef _WIN32
    DWORD_PTR mask = 1ULL << core_id;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#endif
}

} // namespace hft
