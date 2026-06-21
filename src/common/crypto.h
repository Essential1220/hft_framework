#pragma once
// ============================================
// crypto.h - Sensitive data encryption/decryption (敏感数据加密/解密)
//
// Windows: uses DPAPI (CryptProtectData/CryptUnprotectData)
//   - Key is tied to current Windows user, no key management needed
//     (密钥绑定当前 Windows 用户，无需管理密钥)
//
// Linux: uses AES-256-GCM via OpenSSL
//   - Key derived from machine identity: SHA-256(hostname + uid + /etc/machine-id)
//     (密钥从机器身份派生: SHA-256(主机名 + uid + machine-id))
//   - Backward-compatible: auto-detects and decrypts old XOR-obfuscated values
//     (向后兼容: 自动检测并解密旧的 XOR 混淆值)
// ============================================

#include <string>

namespace hft {
namespace crypto {

// Encrypt plaintext, return Base64-encoded ciphertext (加密明文，返回 Base64 编码的密文)
// Returns empty string on failure (失败返回空字符串)
std::string encrypt(const std::string& plaintext);

// Decrypt Base64-encoded ciphertext, return plaintext (解密 Base64 编码的密文，返回明文)
// Returns empty string on failure (失败返回空字符串)
std::string decrypt(const std::string& ciphertext_b64);

// Encrypt a sensitive config.ini value, return "ENC:base64" format (加密 config.ini 中的敏感值，返回 "ENC:base64" 格式)
std::string encrypt_config_value(const std::string& plaintext);

// Decrypt a config.ini value (解密 config.ini 中的敏感值)
// If value starts with "ENC:", decrypt and return plaintext (如果值以 "ENC:" 开头，则解密后返回明文)
// Otherwise return as-is (compatible with unencrypted legacy config) (否则原样返回，兼容未加密的旧配置)
std::string decrypt_config_value(const std::string& value);

} // namespace crypto
} // namespace hft
