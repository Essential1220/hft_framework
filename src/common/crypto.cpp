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
// Linux: AES-256-GCM via OpenSSL, with XOR backward compatibility
// (Linux: 通过 OpenSSL 实现 AES-256-GCM，兼容旧 XOR 格式)

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <unistd.h>
#include <fstream>
#include <array>

namespace hft {
namespace crypto {

static constexpr int kAesKeyLen = 32;   // 256 bits
static constexpr int kGcmIvLen = 12;    // 96-bit IV (NIST recommended)
static constexpr int kGcmTagLen = 16;   // 128-bit auth tag

// Old XOR key for backward compatibility (旧 XOR 密钥，仅用于向后兼容解密)
static const unsigned char kXorKey[] = {
    0x4A, 0x7B, 0x2C, 0x5D, 0x1E, 0x3F, 0x60, 0x81,
    0xA2, 0xC3, 0xE4, 0x05, 0x26, 0x47, 0x68, 0x89
};

static std::string xor_decrypt_legacy(const std::string& ciphertext_b64) {
    auto buf = base64_decode(ciphertext_b64);
    if (buf.empty()) return "";
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] ^= kXorKey[i % sizeof(kXorKey)];
    }
    return std::string(buf.begin(), buf.end());
}

// Derive a 256-bit key from machine identity (从机器身份派生 256 位密钥)
// SHA-256(hostname + ":" + uid + ":" + /etc/machine-id)
static std::array<unsigned char, kAesKeyLen> derive_machine_key() {
    std::string identity;

    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        identity += hostname;
    }
    identity += ":";
    identity += std::to_string(getuid());
    identity += ":";

    std::ifstream mid("/etc/machine-id");
    if (mid.is_open()) {
        std::string line;
        if (std::getline(mid, line)) {
            identity += line;
        }
    }

    std::array<unsigned char, kAesKeyLen> key{};
    SHA256(reinterpret_cast<const unsigned char*>(identity.data()),
           identity.size(), key.data());
    return key;
}

std::string encrypt(const std::string& plaintext) {
    if (plaintext.empty()) return "";

    const auto key = derive_machine_key();

    // Generate random 12-byte IV (生成 12 字节随机 IV)
    unsigned char iv[kGcmIvLen];
    if (RAND_bytes(iv, kGcmIvLen) != 1) return "";

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";

    std::string result;
    int len = 0;
    int ciphertext_len = 0;
    std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    unsigned char tag[kGcmTagLen];

    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
           && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kGcmIvLen, nullptr) == 1
           && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv) == 1
           && EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                  reinterpret_cast<const unsigned char*>(plaintext.data()),
                  static_cast<int>(plaintext.size())) == 1;
    ciphertext_len = len;

    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) == 1;
        ciphertext_len += len;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kGcmTagLen, tag) == 1;
    }

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return "";

    // Output: IV || ciphertext || tag (输出: IV || 密文 || 认证标签)
    std::vector<unsigned char> output;
    output.reserve(kGcmIvLen + ciphertext_len + kGcmTagLen);
    output.insert(output.end(), iv, iv + kGcmIvLen);
    output.insert(output.end(), ciphertext.data(), ciphertext.data() + ciphertext_len);
    output.insert(output.end(), tag, tag + kGcmTagLen);

    return base64_encode(output.data(), output.size());
}

std::string decrypt(const std::string& ciphertext_b64) {
    if (ciphertext_b64.empty()) return "";

    const auto raw = base64_decode(ciphertext_b64);
    // AES-GCM minimum: 12 (IV) + 0 (empty plaintext) + 16 (tag) = 28 bytes
    if (raw.size() < static_cast<size_t>(kGcmIvLen + kGcmTagLen)) {
        return xor_decrypt_legacy(ciphertext_b64);
    }

    const auto key = derive_machine_key();
    const unsigned char* iv = raw.data();
    const int ciphertext_len = static_cast<int>(raw.size()) - kGcmIvLen - kGcmTagLen;
    const unsigned char* ciphertext_data = raw.data() + kGcmIvLen;
    const unsigned char* tag = raw.data() + raw.size() - kGcmTagLen;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return xor_decrypt_legacy(ciphertext_b64);

    std::vector<unsigned char> plaintext(ciphertext_len + EVP_MAX_BLOCK_LENGTH);
    int len = 0;
    int plaintext_len = 0;

    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
           && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kGcmIvLen, nullptr) == 1
           && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv) == 1;

    if (ok && ciphertext_len > 0) {
        ok = EVP_DecryptUpdate(ctx, plaintext.data(), &len,
                               ciphertext_data, ciphertext_len) == 1;
        plaintext_len = len;
    }

    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kGcmTagLen,
                                 const_cast<unsigned char*>(tag)) == 1;
    }
    if (ok) {
        ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + plaintext_len, &len) == 1;
        plaintext_len += len;
    }

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        // GCM auth failed — try legacy XOR decryption
        // (GCM 认证失败，回退到旧 XOR 解密)
        return xor_decrypt_legacy(ciphertext_b64);
    }

    return std::string(reinterpret_cast<char*>(plaintext.data()), plaintext_len);
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
