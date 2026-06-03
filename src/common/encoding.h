#pragma once
// ============================================
// encoding.h - GBK to UTF-8 transcoding (GBK → UTF-8 转码)
// CTP API returns GBK-encoded Chinese on both Windows and Linux (CTP API 无论 Windows 还是 Linux 均返回 GBK 编码的中文)
// Windows: uses WinAPI MultiByteToWideChar (Windows: 使用 WinAPI MultiByteToWideChar)
// Linux:   uses POSIX iconv (Linux: 使用 POSIX iconv)
// ============================================

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef ERROR
#else
#include <iconv.h>
#include <cerrno>
#include <cstring>
#endif

#include <string>

namespace hft {

// Convert GBK-encoded string to UTF-8 (将 GBK 编码的字符串转换为 UTF-8 编码的字符串)
inline std::string gbk_to_utf8(const char* gbk) {
    if (!gbk || !gbk[0]) return {}; // Null or empty string returns empty (空指针或空字符串直接返回空字符串)

#ifdef _WIN32
    // Windows implementation: using MultiByteToWideChar and WideCharToMultiByte
    // (Windows 平台实现：使用 MultiByteToWideChar 和 WideCharToMultiByte)

    // Step 1: get required length for wide string (936 = GBK code page)
    // (第一步：获取转换后宽字符串需要的长度，936 代表 GBK 代码页)
    int wlen = MultiByteToWideChar(936, 0, gbk, -1, nullptr, 0);
    if (wlen <= 0) return gbk; // Conversion failed, return as-is (转换失败则原样返回)

    // Allocate wide string and convert (GBK -> UTF-16) (分配宽字符串内存并进行转换)
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(936, 0, gbk, -1, &wstr[0], wlen);

    // Step 2: get required byte count for UTF-8 (第二步：获取转换为 UTF-8 后需要的字节数)
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return gbk; // Conversion failed, return as-is (转换失败则原样返回)

    // Allocate UTF-8 string and convert (UTF-16 -> UTF-8) (分配 UTF-8 字符串内存并进行转换)
    std::string utf8(ulen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8[0], ulen, nullptr, nullptr);

    // Remove trailing '\0' to match std::string length convention (剔除末尾的 '\0'，以符合 std::string 的长度习惯)
    utf8.resize(ulen - 1);
    return utf8;
#else
    // Linux/POSIX implementation: using iconv (Linux/POSIX 平台实现：使用 iconv)

    // Open iconv conversion descriptor: target UTF-8, source GBK (打开 iconv 转换句柄，目标 UTF-8，源 GBK)
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        // iconv doesn't support GBK (rare), return raw bytes (iconv 不支持 GBK，极少见，直接返回原始字节)
        return std::string(gbk);
    }

    // Prepare input buffer (准备输入缓冲)
    std::string src(gbk);
    size_t in_left = src.size();
    char* in_ptr = &src[0];

    // Prepare output buffer (准备输出缓冲)
    // In UTF-8 one Chinese character is at most 3 bytes, GBK uses 2 bytes.
    // So at most 3/2 of GBK size; reserve enough space to avoid truncation
    // (UTF-8 编码中一个中文字符最多占 3 个字节，GBK 占 2 个字节，预留足够空间)
    std::string dst(in_left * 2 + 4, '\0');
    size_t out_left = dst.size();
    char* out_ptr = &dst[0];

    // Execute conversion (执行转换)
    const size_t ret = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
    iconv_close(cd); // Close descriptor (关闭句柄)

    if (ret == static_cast<size_t>(-1)) {
        // Conversion failed (e.g. invalid sequence), return raw bytes to avoid crash
        // (转换失败，如遇到无效序列，返回原始字节，避免崩溃)
        return std::string(gbk);
    }

    // Truncate according to consumed buffer size to get the actual result
    // (根据消耗的缓冲区大小截断字符串，得到实际的转换结果)
    dst.resize(dst.size() - out_left);
    return dst;
#endif
}

} // namespace hft
