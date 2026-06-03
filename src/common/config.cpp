// ============================================
// config.cpp - INI config parser (INI 配置文件解析器)
// ============================================

#include "common/config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <tuple>

namespace hft {

namespace {

 // Trim leading and trailing whitespace (去除字符串首尾的空白字符)
std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1); // Extract valid portion (截取有效部分)
}

 // Resolve environment variable reference, format "env:ENV_NAME" (解析环境变量引用，格式为 "env:ENV_NAME")
std::string resolve_env_reference(const std::string& value) {
    constexpr const char* kEnvPrefix = "env:";
    if (value.rfind(kEnvPrefix, 0) != 0) {
        return value;
    }

    const std::string env_name = trim(value.substr(4));
    if (env_name.empty()) {
        return ""; // Empty env var name, return empty string (环境变量名为空，返回空字符串)
    }

    const char* env_value = std::getenv(env_name.c_str());
    return env_value ? std::string(env_value) : std::string();
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool is_account_gateway_section(const std::string& section) {
    const auto dot = section.find('.');
    if (dot == std::string::npos) {
        return false;
    }

    const std::string prefix = section.substr(0, dot);
    return !prefix.empty() && prefix != "Strategy";
}

int section_rank(const std::string& section) {
    if (section == "Accounts") return 0;
    if (is_account_gateway_section(section)) return 10;
    if (section == "Strategies") return 20;
    if (starts_with(section, "Strategy.")) return 30;
    if (section == "Risk") return 40;
    if (section == "Trading") return 50;
    if (section == "Performance") return 60;
    if (section == "Log") return 70;
    if (section == "Web") return 80;
    if (section == "Runtime") return 90;
    if (section == "CTP") return 100;
    if (section == "MD") return 110;
    return 1000;
}

int key_rank(const std::string& section, const std::string& key) {
    if (section == "Accounts") {
        if (key == "List") return 0;
        if (key == "MarketDataAccount") return 1;
        if (key.find(".Gateway") != std::string::npos) return 10;
    } else if (is_account_gateway_section(section) || section == "CTP" || section == "MD") {
        if (key == "BrokerID") return 0;
        if (key == "UserID") return 1;
        if (key == "Password") return 2;
        if (key == "AppID") return 3;
        if (key == "AuthCode") return 4;
        if (key == "TradeFront") return 5;
        if (key == "MarketFront") return 6;
    } else if (section == "Strategies") {
        if (key == "List") return 0;
    } else if (starts_with(section, "Strategy.") || section == "Strategy") {
        if (key == "Type") return 0;
        if (key == "ScriptPath") return 1;
        if (key == "AccountID") return 2;
        if (key == "Instruments") return 3;
        if (key == "OrderSize") return 4;
        if (key == "MomentumTicks") return 5;
        if (key == "CooldownSeconds") return 6;
        if (starts_with(key, "Param.")) return 20;
    } else if (section == "Risk") {
        if (key == "MaxOrderSize") return 0;
        if (key == "MaxNetPosition") return 1;
        if (key == "MaxOrdersPerMinute") return 2;
        if (key == "MaxCancelRate") return 3;
        if (key == "MaxDailyLoss") return 4;
        if (key == "CancelRateWindowMinutes") return 5;
    } else if (section == "Trading") {
        if (key == "TradingSessions") return 0;
    } else if (section == "Performance") {
        if (key == "EngineCpuCore") return 0;
        if (key == "LoggerCpuCore") return 1;
        if (key == "ProductionHftMode") return 2;
        if (key == "MdBatchSize") return 3;
        if (key == "DisablePythonHotPath") return 4;
        if (key == "DisableTickRecordingHotPath") return 5;
        if (key == "DisableKlineHotPath") return 6;
    } else if (section == "Log") {
        if (key == "Level") return 0;
        if (key == "Directory") return 1;
        if (key == "FilePrefix") return 2;
        if (key == "QueueCapacity") return 3;
        if (key == "RecentBufferSize") return 4;
        if (key == "FlushIntervalMs") return 5;
        if (key == "RetentionDays") return 6;
    } else if (section == "Web") {
        if (key == "Host") return 0;
        if (key == "Port") return 1;
        if (key == "AuthToken") return 2;
        if (key == "AllowedOrigins") return 3;
        if (key == "AllowInsecureNoAuth") return 4;
        if (key == "AutoOpenBrowser") return 5;
    } else if (section == "Runtime") {
        if (key == "RunMode") return 0;
        if (key == "StateFile") return 1;
        if (key == "NoTickWarnSeconds") return 2;
    }

    return 1000;
}

} // namespace

bool Config::load(const std::string& filepath) {
    data_.clear();

    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false; // File open failed (文件打开失败)
    }

    std::string line;
    std::string current_section;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
            line = trim(line.substr(3));
        }
        // Skip empty lines and comment lines starting with # or ; (忽略空行和注释行，以 # 或 ; 开头)
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Parse section, e.g. [CTP] (解析 section，如 [CTP])
        if (line.front() == '[' && line.back() == ']') {
            current_section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        // Parse key=value (解析 key=value)
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue; // No equals sign, malformed, skip (没有等号，格式错误，跳过)
        }

        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        data_[current_section][key] = val; // Store in 2D map (存入二维 map)
    }

    return true; // Load succeeded (加载成功)
}

std::string Config::get_string(const std::string& section, const std::string& key,
                               const std::string& default_val) const {
    const auto sit = data_.find(section);
    if (sit == data_.end()) {
        return default_val;
    }

    const auto kit = sit->second.find(key);
    if (kit == sit->second.end()) {
        return default_val;
    }

    // Try to resolve environment variable reference (尝试解析环境变量引用)
    const std::string resolved = resolve_env_reference(kit->second);
    if (resolved.empty() && !kit->second.empty() && kit->second.rfind("env:", 0) == 0) {
        return default_val;
    }
    return resolved;
}

int Config::get_int(const std::string& section, const std::string& key, int default_val) const {
    const std::string val = get_string(section, key);
    if (val.empty()) {
        return default_val;
    }
    try {
        return std::stoi(val);
    } catch (...) {
        return default_val;
    }
}

double Config::get_double(const std::string& section, const std::string& key, double default_val) const {
    const std::string val = get_string(section, key);
    if (val.empty()) {
        return default_val;
    }
    try {
        return std::stod(val); // Convert to double (转换为浮点数)
    } catch (...) {
        return default_val;
    }
}

bool Config::has_section(const std::string& section) const {
    return data_.find(section) != data_.end(); // Check if section exists (查找 section 是否存在)
}

std::vector<std::string> Config::get_sections(const std::string& prefix) const {
    std::vector<std::string> result;
    for (const auto& [name, _] : data_) {
        if (prefix.empty() || name.compare(0, prefix.size(), prefix) == 0) {
            result.push_back(name); // Add to result list (加入结果列表)
        }
    }
    return result;
}

std::map<std::string, std::string> Config::get_items(const std::string& section) const {
    const auto it = data_.find(section);
    if (it == data_.end()) {
        return {};
    }
    return it->second;
}

void Config::set_string(const std::string& section, const std::string& key, std::string value) {
    data_[section][key] = std::move(value);
}

void Config::erase_key(const std::string& section, const std::string& key) {
    const auto sit = data_.find(section);
    if (sit == data_.end()) {
        return;
    }

    sit->second.erase(key);
    if (sit->second.empty()) {
        data_.erase(sit);
    }
}

void Config::erase_section(const std::string& section) {
    data_.erase(section);
}

bool Config::save(const std::string& filepath) const {
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path path(filepath);
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream file(filepath, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    std::vector<std::pair<std::string, std::map<std::string, std::string>>> ordered_sections;
    ordered_sections.reserve(data_.size());
    for (const auto& item : data_) {
        ordered_sections.push_back(item);
    }
    std::sort(ordered_sections.begin(), ordered_sections.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::make_tuple(section_rank(lhs.first), lhs.first) <
                         std::make_tuple(section_rank(rhs.first), rhs.first);
              });

    bool first_section = true;
    for (const auto& [section, items] : ordered_sections) {
        if (!first_section) {
            file << "\n";
        }
        first_section = false;

        file << "[" << section << "]\n";
        std::vector<std::pair<std::string, std::string>> ordered_items(items.begin(), items.end());
        std::sort(ordered_items.begin(), ordered_items.end(),
                  [&section](const auto& lhs, const auto& rhs) {
                      return std::make_tuple(key_rank(section, lhs.first), lhs.first) <
                             std::make_tuple(key_rank(section, rhs.first), rhs.first);
                  });

        for (const auto& [key, value] : ordered_items) {
            file << key << " = " << value << "\n";
        }
    }

    return file.good();
}

} // namespace hft




