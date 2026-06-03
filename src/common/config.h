#pragma once
// ============================================
// config.h - INI config file reader (INI 配置文件读取器)
// Pure standard library implementation, no Qt or third-party dependencies (纯标准库实现，不依赖 Qt 或第三方库)
// ============================================

#include <string>
#include <map>
#include <vector>

namespace hft {

class Config {
public:
    // Load INI file, return true on success (加载 INI 文件，成功返回 true)
    bool load(const std::string& filepath);

    // Read string value, return default_val if not found. Supports env: prefix for env vars
    // (读取字符串值，找不到返回 default_val。支持 env: 前缀解析环境变量)
    std::string get_string(const std::string& section, const std::string& key, const std::string& default_val = "") const;

    // Read integer value. Returns default_val if not found or parse error (读取整数值，不存在或格式错误返回 default_val)
    int get_int(const std::string& section, const std::string& key, int default_val = 0) const;

    // Read double value. Returns default_val if not found or parse error (读取浮点值，不存在或格式错误返回 default_val)
    double get_double(const std::string& section, const std::string& key, double default_val = 0.0) const;

    // Check if a section exists (判断某个 section 是否存在)
    bool has_section(const std::string& section) const;

    // Get all section names starting with prefix (获取以 prefix 开头的所有 section 名称)
    // If prefix is empty, returns all sections (如果 prefix 为空，则返回所有 section)
    // Example: "CTP." -> ["CTP.Account1", "CTP.Account2"] (示例："CTP." -> ["CTP.Account1", "CTP.Account2"])
    std::vector<std::string> get_sections(const std::string& prefix = "") const;

    // Get all key-value pairs for a section (获取某个 section 下的全部键值对)
    std::map<std::string, std::string> get_items(const std::string& section) const;

    // Set or overwrite a config entry (设置或覆盖配置项)
    void set_string(const std::string& section, const std::string& key, std::string value);

    // Delete a specific config key (删除指定配置项)
    void erase_key(const std::string& section, const std::string& key);

    // Delete an entire config section (删除整个配置节)
    void erase_section(const std::string& section);

    // Save INI file, return true on success (保存 INI 文件，成功返回 true)
    bool save(const std::string& filepath) const;

private:
    // Two-level map: stores all config data (二级 map：存储所有的配置项数据)
    // Structure: section_name -> (key -> value) (结构：section_name -> (key -> value))
    std::map<std::string, std::map<std::string, std::string>> data_;
};

} // namespace hft
