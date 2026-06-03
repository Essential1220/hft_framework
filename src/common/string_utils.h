#pragma once
// ============================================
// string_utils.h - String/CSV/JSON utility helpers (字符串/CSV/JSON 工具函数)
// ============================================

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace hft {

// Trim leading and trailing whitespace (去除首尾空白字符)
inline std::string trim_copy(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Convert string to lowercase (字符串转小写)
inline std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

// Sanitize a string for use as a filename component (将字符串净化为安全的文件名片段)
inline std::string safe_path_component(std::string value) {
    value = trim_copy(value);
    for (char& ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.';
        if (!ok) ch = '_';
    }
    return value.empty() ? "UNKNOWN" : value;
}

// Split comma-separated string, trim each token (按逗号分割字符串并去除每项空白)
inline std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream iss(text);
    std::string token;
    while (std::getline(iss, token, ',')) {
        token = trim_copy(token);
        if (!token.empty()) {
            result.push_back(token);
        }
    }
    return result;
}

// Escape special characters for JSON string value (转义 JSON 字符串中的特殊字符)
inline std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Case-insensitive file extension check (不区分大小写检查文件扩展名)
inline bool has_extension_ci(const std::filesystem::path& path, const std::string& ext) {
    return lower_copy(path.extension().string()) == lower_copy(ext);
}

} // namespace hft
