// ============================================
// crypto.cpp - DPAPI encryption implementation (DPAPI 加密实现)
// ============================================

#include "common/crypto.h"

#include <cstring>
#include <string>
#include <vector>

// Base64 encode/decode (inline implementation, no external dependencies)
// (Base64 编解码，内联实现，无外部依赖)
namespace {

static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const unsigned int b0 = data[i];
        const unsigned int b1 = (i + 1 < len) ? data[i + 1] : 0;
        const unsigned int b2 = (i + 2 < len) ? data[i + 2] : 0;
        result += kBase64Chars[(b0 >> 2) & 0x3F];
        result += kBase64Chars[((b0 << 4) | (b1 >> 4)) & 0x3F];
        result += (i + 1 < len) ? kBase64Chars[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=';
        result += (i + 2 < len) ? kBase64Chars[b2 & 0x3F] : '=';
    }
    return result;
}

int base64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<unsigned char> base64_decode(const std::string& encoded) {
    std::vector<unsigned char> result;
    result.reserve(encoded.size() * 3 / 4);
    int val = 0, bits = -8;
    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int d = base64_decode_char(c);
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            result.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return result;
}

} // namespace

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

namespace hft {
namespace crypto {

std::string encrypt(const std::string& plaintext) {
    if (plaintext.empty()) return "";

    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output;
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        return "";
    }

    std::string result = base64_encode(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::string decrypt(const std::string& ciphertext_b64) {
    if (ciphertext_b64.empty()) return "";

    const auto raw = base64_decode(ciphertext_b64);
    if (raw.empty()) return "";

    DATA_BLOB input;
    input.pbData = const_cast<unsigned char*>(raw.data());
    input.cbData = static_cast<DWORD>(raw.size());

    DATA_BLOB output;
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        return "";
    }

    std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

} // namespace crypto
} // namespace hft

#else
// Non-Windows: XOR obfuscation (production should replace with AES-256-GCM)
// (非 Windows: XOR 混淆，生产环境应替换为 AES-256-GCM)
namespace hft {
namespace crypto {

static const unsigned char kXorKey[] = {
    0x4A, 0x7B, 0x2C, 0x5D, 0x1E, 0x3F, 0x60, 0x81,
    0xA2, 0xC3, 0xE4, 0x05, 0x26, 0x47, 0x68, 0x89
};

std::string encrypt(const std::string& plaintext) {
    if (plaintext.empty()) return "";
    std::vector<unsigned char> buf(plaintext.begin(), plaintext.end());
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] ^= kXorKey[i % sizeof(kXorKey)];
    }
    return base64_encode(buf.data(), buf.size());
}

std::string decrypt(const std::string& ciphertext_b64) {
    if (ciphertext_b64.empty()) return "";
    auto buf = base64_decode(ciphertext_b64);
    if (buf.empty()) return "";
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] ^= kXorKey[i % sizeof(kXorKey)];
    }
    return std::string(buf.begin(), buf.end());
}

} // namespace crypto
} // namespace hft

#endif

// Platform-independent config value helpers (use platform-specific encrypt/decrypt above)
// (跨平台配置值辅助函数，使用上方的平台特定加密/解密实现)
namespace hft {
namespace crypto {

static constexpr const char* kEncPrefix = "ENC:";
static constexpr size_t kEncPrefixLen = 4;

std::string encrypt_config_value(const std::string& plaintext) {
    if (plaintext.empty()) return "";
    const std::string enc = encrypt(plaintext);
    if (enc.empty()) return plaintext;
    return std::string(kEncPrefix) + enc;
}

std::string decrypt_config_value(const std::string& value) {
    if (value.empty()) return "";
    if (value.size() > kEncPrefixLen &&
        value.compare(0, kEncPrefixLen, kEncPrefix) == 0) {
        const std::string dec = decrypt(value.substr(kEncPrefixLen));
        if (!dec.empty()) return dec;
    }
    return value;
}

} // namespace crypto
} // namespace hft
