#pragma once
// ============================================
// strategy_config.h - Strategy configuration loading and validation (策略配置加载与校验)
//
// Parses strategy specs from INI-style config files, validates field constraints,
// and normalizes runtime parameters for strategy initialization.
// 从 INI 格式配置文件解析策略规格，校验字段约束，标准化运行时参数。
// ============================================

#include "common/config.h"

#include <map>
#include <string>
#include <vector>

namespace hft {

// Describes a single strategy instance as configured in the config file
// 描述配置文件中定义的单个策略实例 (策略配置规格)
struct StrategyConfigSpec {
    std::string id;                        // Unique strategy identifier (策略唯一标识)
    std::string type;                      // Strategy type: "simple" or "python" (策略类型)
    std::string script_path;               // Path to Python script (if type=python) (Python 脚本路径)
    std::string account_id;                // Associated account ID (关联的资金账号)
    std::string version;                   // Strategy version string (策略版本)
    std::vector<std::string> instruments;  // Instruments this strategy watches (策略关注的合约列表)
    int order_size = 1;                    // Default order size in lots (默认下单手数)
    int momentum_ticks = 3;                // Number of ticks for momentum calculation (动量计算 Tick 数)
    int cooldown_seconds = 5;              // Cooldown period after signal in seconds (信号冷却时间/秒)
    std::map<std::string, std::string> params; // Additional key-value parameters (扩展键值参数)
    std::string source_section;            // Config section this spec was loaded from (来源配置段)
};

// Validation error details (校验错误详情)
struct StrategyConfigValidationError {
    std::string field;   // Field name that failed validation (校验失败的字段名)
    std::string message; // Human-readable error description (人类可读的错误描述)
};

// Split a comma-separated string, trimming whitespace from each element
// 按逗号分割字符串，去除每个元素前后空白 (拆分CSV并去空格)
std::vector<std::string> split_csv_trimmed(const std::string& text);
// Join a vector of strings into a comma-separated string
// 将字符串数组拼接为逗号分隔的字符串 (拼接为CSV)
std::string join_csv(const std::vector<std::string>& values);
// Load all strategy specifications from the config
// 从配置中加载所有策略规格 (加载策略配置)
std::vector<StrategyConfigSpec> load_strategy_specs(const Config& config);
// Load configured account IDs from the config
// 从配置中加载已配置的资金账号列表 (加载账号列表)
std::vector<std::string> load_configured_account_ids(const Config& config);
// Resolve a script path relative to the config file directory
// 将脚本路径相对于配置文件目录解析为绝对路径 (解析脚本路径)
std::string resolve_strategy_script_path(const std::string& config_path, const std::string& script_path);
// Build a runtime parameter map combining built-in fields and custom params
// 将内置字段和自定义参数合并为运行时参数映射表 (构建运行时参数表)
std::map<std::string, std::string> build_runtime_param_map(const StrategyConfigSpec& spec);
// Normalize and validate all strategy specs; returns false on first error
// 标准化并校验所有策略规格，首次出错返回 false (标准化和校验策略配置)
bool normalize_and_validate_strategy_specs(std::vector<StrategyConfigSpec>& specs,
                                           StrategyConfigValidationError* error = nullptr);
// Save strategy specs back to config (used by hot-reload / admin tools)
// 将策略规格保存回配置（用于热重载/管理工具）(保存策略配置)
void save_strategy_specs(Config& config, const std::vector<StrategyConfigSpec>& specs);

} // namespace hft
