#pragma once

#include <cstddef>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace hft {

inline bool enable_lock_memory_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    TOKEN_PRIVILEGES tp{};
    if (!LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

inline void* allocate_huge_page(size_t size) {
    SIZE_T large_min = GetLargePageMinimum();
    if (large_min == 0) return nullptr;
    size = (size + large_min - 1) & ~(large_min - 1);
    return VirtualAlloc(nullptr, size,
                        MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                        PAGE_READWRITE);
}

inline void free_huge_page(void* ptr) {
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

} // namespace hft

#else

namespace hft {

inline bool enable_lock_memory_privilege() { return false; }
inline void* allocate_huge_page(size_t) { return nullptr; }
inline void free_huge_page(void*) {}

} // namespace hft

#endif
