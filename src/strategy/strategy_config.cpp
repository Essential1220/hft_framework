// ============================================
// strategy_config.cpp - Strategy config loading, normalization, and validation (策略配置加载/标准化/校验)
// ============================================

#include "strategy/strategy_config.h"

#include "common/string_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace hft {

namespace {

StrategyConfigSpec load_strategy_spec(const Config& config,
                                      const std::string& section,
                                      const std::string& fallback_id) {
    StrategyConfigSpec spec;
    spec.id = fallback_id;
    spec.source_section = section;
    spec.type = trim_copy(config.get_string(section, "Type", ""));
    spec.script_path = trim_copy(config.get_string(section, "ScriptPath", ""));
    spec.account_id = trim_copy(config.get_string(section, "AccountID", ""));
    spec.version = trim_copy(config.get_string(section, "Version", ""));
    spec.instruments = split_csv_trimmed(config.get_string(section, "Instruments", ""));
    spec.order_size = config.get_int(section, "OrderSize", 1);
    spec.momentum_ticks = config.get_int(section, "MomentumTicks", 3);
    spec.cooldown_seconds = config.get_int(section, "CooldownSeconds", 5);

    if (spec.type.empty()) {
        spec.type = spec.script_path.empty() ? "simple" : "python";
    }
    std::transform(spec.type.begin(), spec.type.end(), spec.type.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (const auto& [key, value] : config.get_items(section)) {
        if (key.rfind("Param.", 0) == 0 && key.size() > 6) {
            spec.params[key.substr(6)] = value;
        }
    }

    return spec;
}

bool set_validation_error(StrategyConfigValidationError* error,
                          std::string field,
                          std::string message) {
    if (error) {
        error->field = std::move(field);
        error->message = std::move(message);
    }
    return false;
}

bool try_parse_double_strict(const std::string& text, double* out_value = nullptr) {
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return false;
    }

    size_t pos = 0;
    double parsed = 0.0;
    try {
        parsed = std::stod(trimmed, &pos);
    } catch (...) {
        return false;
    }
    if (pos != trimmed.size()) {
        return false;
    }
    if (out_value) {
        *out_value = parsed;
    }
    return true;
}

void normalize_instruments(std::vector<std::string>& instruments) {
    std::vector<std::string> normalized;
    normalized.reserve(instruments.size());
    for (auto& instrument : instruments) {
        const std::string trimmed = trim_copy(instrument);
        if (trimmed.empty()) {
            continue;
        }
        if (std::find(normalized.begin(), normalized.end(), trimmed) == normalized.end()) {
            normalized.push_back(trimmed);
        }
    }
    instruments = std::move(normalized);
}

void normalize_misc_runtime_params(std::map<std::string, std::string>& params) {
    params.erase("OrderSize");
    params.erase("MomentumTicks");
    params.erase("CooldownSeconds");
    params.erase("order_size");
    params.erase("momentum_ticks");
    params.erase("cooldown_seconds");
}

bool normalize_optional_positive_param(std::map<std::string, std::string>& params,
                                       const std::vector<std::string>& aliases,
                                       const std::string& canonical_key,
                                       const std::string& field_name,
                                       const std::string& strategy_id,
                                       StrategyConfigValidationError* error) {
    std::string normalized_value;
    bool found = false;

    for (const auto& alias : aliases) {
        const auto it = params.find(alias);
        if (it == params.end()) {
            continue;
        }

        const std::string value = trim_copy(it->second);
        if (value.empty()) {
            continue;
        }

        if (!found) {
            normalized_value = value;
            found = true;
            continue;
        }

        if (value != normalized_value) {
            return set_validation_error(
                error,
                field_name,
                "strategy " + strategy_id + " has conflicting aliases for " + canonical_key);
        }
    }

    for (const auto& alias : aliases) {
        params.erase(alias);
    }

    if (!found) {
        return true;
    }

    double parsed = 0.0;
    if (!try_parse_double_strict(normalized_value, &parsed)) {
        return set_validation_error(
            error,
            field_name,
            "strategy " + strategy_id + " has invalid numeric value for " + canonical_key);
    }
    if (parsed <= 0.0) {
        return set_validation_error(
            error,
            field_name,
            "strategy " + strategy_id + " requires " + canonical_key + " > 0");
    }

    params[canonical_key] = normalized_value;
    return true;
}

bool normalize_and_validate_strategy_spec(StrategyConfigSpec& spec,
                                          StrategyConfigValidationError* error) {
    spec.id = trim_copy(spec.id);
    spec.type = trim_copy(spec.type);
    spec.script_path = trim_copy(spec.script_path);
    spec.account_id = trim_copy(spec.account_id);
    normalize_instruments(spec.instruments);

    std::transform(spec.type.begin(), spec.type.end(), spec.type.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (spec.type.empty()) {
        spec.type = spec.script_path.empty() ? "simple" : "python";
    }

    if (spec.type != "python" && spec.type != "simple") {
        return set_validation_error(
            error, "type", "strategy " + spec.id + " has unsupported type: " + spec.type);
    }
    if (spec.type == "python" && spec.script_path.empty()) {
        return set_validation_error(
            error, "script_path", "strategy " + spec.id + " is missing ScriptPath");
    }
    if (spec.order_size <= 0) {
        return set_validation_error(
            error, "order_size", "strategy " + spec.id + " requires order_size >= 1");
    }
    if (spec.momentum_ticks <= 0) {
        return set_validation_error(
            error, "momentum_ticks", "strategy " + spec.id + " requires momentum_ticks >= 1");
    }
    if (spec.cooldown_seconds < 0) {
        return set_validation_error(
            error, "cooldown_seconds", "strategy " + spec.id + " requires cooldown_seconds >= 0");
    }

    normalize_misc_runtime_params(spec.params);

    if (!normalize_optional_positive_param(
            spec.params,
            {"StopLossOffset", "stop_loss_offset"},
            "StopLossOffset",
            "stop_loss_offset",
            spec.id,
            error)) {
        return false;
    }
    if (!normalize_optional_positive_param(
            spec.params,
            {"TakeProfitOffset", "take_profit_offset"},
            "TakeProfitOffset",
            "take_profit_offset",
            spec.id,
            error)) {
        return false;
    }
    if (!normalize_optional_positive_param(
            spec.params,
            {"TrailOffset", "trail_offset"},
            "TrailOffset",
            "trail_offset",
            spec.id,
            error)) {
        return false;
    }

    return true;
}

} // namespace

std::vector<std::string> split_csv_trimmed(const std::string& text) {
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

std::string join_csv(const std::vector<std::string>& values) {
    std::ostringstream ss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            ss << ", ";
        }
        ss << values[i];
    }
    return ss.str();
}

std::vector<StrategyConfigSpec> load_strategy_specs(const Config& config) {
    std::vector<StrategyConfigSpec> specs;
    if (config.has_section("Strategies")) {
        for (const auto& id : split_csv_trimmed(config.get_string("Strategies", "List", ""))) {
            specs.push_back(load_strategy_spec(config, "Strategy." + id, id));
        }
        return specs;
    }

    specs.push_back(load_strategy_spec(config, "Strategy", "default"));
    return specs;
}

std::vector<std::string> load_configured_account_ids(const Config& config) {
    if (config.has_section("Accounts")) {
        return split_csv_trimmed(config.get_string("Accounts", "List", ""));
    }

    const std::string single = trim_copy(config.get_string("CTP", "UserID", ""));
    if (single.empty()) {
        return {};
    }
    return {single};
}

std::string resolve_strategy_script_path(const std::string& config_path, const std::string& script_path) {
    if (script_path.empty()) {
        return "";
    }

    namespace fs = std::filesystem;
    fs::path path(script_path);
    if (path.is_absolute()) {
        return path.lexically_normal().string();
    }

    const fs::path base_dir = fs::path(config_path).parent_path();
    return (base_dir / path).lexically_normal().string();
}

std::map<std::string, std::string> build_runtime_param_map(const StrategyConfigSpec& spec) {
    std::map<std::string, std::string> params = spec.params;
    params["OrderSize"] = std::to_string(spec.order_size);
    params["MomentumTicks"] = std::to_string(spec.momentum_ticks);
    params["CooldownSeconds"] = std::to_string(spec.cooldown_seconds);
    return params;
}

bool normalize_and_validate_strategy_specs(std::vector<StrategyConfigSpec>& specs,
                                           StrategyConfigValidationError* error) {
    for (auto& spec : specs) {
        if (!normalize_and_validate_strategy_spec(spec, error)) {
            return false;
        }
    }
    return true;
}

void save_strategy_specs(Config& config, const std::vector<StrategyConfigSpec>& specs) {
    config.erase_section("Strategy");
    config.erase_section("Strategies");
    for (const auto& section : config.get_sections("Strategy.")) {
        config.erase_section(section);
    }

    std::vector<std::string> ids;
    ids.reserve(specs.size());
    for (const auto& spec : specs) {
        ids.push_back(spec.id);
    }
    config.set_string("Strategies", "List", join_csv(ids));

    for (const auto& spec : specs) {
        const std::string section = "Strategy." + spec.id;
        config.set_string(section, "Type", spec.type);
        config.set_string(section, "ScriptPath", spec.script_path);
        config.set_string(section, "AccountID", spec.account_id);
        if (!spec.version.empty()) {
            config.set_string(section, "Version", spec.version);
        }
        config.set_string(section, "Instruments", join_csv(spec.instruments));
        config.set_string(section, "OrderSize", std::to_string(spec.order_size));
        config.set_string(section, "MomentumTicks", std::to_string(spec.momentum_ticks));
        config.set_string(section, "CooldownSeconds", std::to_string(spec.cooldown_seconds));

        for (const auto& [key, value] : spec.params) {
            config.set_string(section, "Param." + key, value);
        }
    }
}

} // namespace hft
