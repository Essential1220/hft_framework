#include "common/config_validator.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace hft {

namespace {

void add_error(std::vector<ConfigValidationError>& errors,
               const std::string& section, const std::string& key,
               const std::string& message) {
    errors.push_back({section, key, message});
}

bool is_positive_int(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool is_integer(const std::string& s) {
    if (s.empty()) return false;
    size_t start = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (start >= s.size()) return false;
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    bool has_dot = false;
    size_t start = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (start >= s.size()) return false;
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] == '.') {
            if (has_dot) return false;
            has_dot = true;
        } else if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t start = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (start != std::string::npos) {
            result.push_back(item.substr(start, end - start + 1));
        }
    }
    return result;
}

} // namespace

bool ConfigValidator::validate(const Config& config,
                               std::vector<ConfigValidationError>& errors) const {
    errors.clear();
    check_accounts(config, errors);
    check_risk(config, errors);
    check_strategies(config, errors);
    check_log(config, errors);
    check_web(config, errors);
    check_performance(config, errors);
    check_runtime(config, errors);
    return errors.empty();
}

void ConfigValidator::check_accounts(const Config& config,
                                     std::vector<ConfigValidationError>& errors) const {
    const auto list_str = config.get_string("Accounts", "List", "");
    if (list_str.empty()) {
        add_error(errors, "Accounts", "List", "account list is empty, no accounts will be loaded");
        return;
    }

    const auto account_ids = split_csv(list_str);
    const auto md_account = config.get_string("Accounts", "MarketDataAccount", "");

    if (md_account.empty()) {
        add_error(errors, "Accounts", "MarketDataAccount", "market data account not specified");
    } else {
        bool md_found = false;
        for (const auto& id : account_ids) {
            if (id == md_account) { md_found = true; break; }
        }
        if (!md_found) {
            add_error(errors, "Accounts", "MarketDataAccount",
                      "MarketDataAccount '" + md_account + "' is not in Accounts.List");
        }
    }

    for (const auto& id : account_ids) {
        const auto gateway_type = config.get_string("Accounts", id + ".Gateway", "CTP");
        std::string section;

        if (gateway_type == "CTP" || gateway_type == "CTP_DUAL") {
            section = "CTP." + id;
        } else if (gateway_type == "QDP") {
            section = "QDP." + id;
        } else if (gateway_type == "FIX") {
            section = "FIX." + id;
        } else if (gateway_type == "UDP" || gateway_type == "SHM") {
            continue;
        } else {
            add_error(errors, "Accounts", id + ".Gateway",
                      "unknown gateway type '" + gateway_type + "'");
            continue;
        }

        if (!config.has_section(section)) {
            add_error(errors, section, "", "account '" + id + "' requires section [" + section + "]");
            continue;
        }

        if (config.get_string(section, "BrokerID", "").empty()) {
            add_error(errors, section, "BrokerID", "BrokerID is required");
        }
        if (config.get_string(section, "UserID", "").empty()) {
            add_error(errors, section, "UserID", "UserID is required");
        }
        if (config.get_string(section, "Password", "").empty()) {
            add_error(errors, section, "Password", "Password is required");
        }
        if (config.get_string(section, "TradeFront", "").empty()) {
            add_error(errors, section, "TradeFront", "TradeFront address is required");
        }
        if (config.get_string(section, "MarketFront", "").empty()) {
            add_error(errors, section, "MarketFront", "MarketFront address is required");
        }
    }
}

void ConfigValidator::check_risk(const Config& config,
                                 std::vector<ConfigValidationError>& errors) const {
    if (!config.has_section("Risk")) return;

    const auto max_order = config.get_string("Risk", "MaxOrderSize", "");
    if (!max_order.empty()) {
        if (!is_positive_int(max_order) || std::stoi(max_order) <= 0) {
            add_error(errors, "Risk", "MaxOrderSize", "must be a positive integer, got '" + max_order + "'");
        }
    }

    const auto max_pos = config.get_string("Risk", "MaxNetPosition", "");
    if (!max_pos.empty()) {
        if (!is_positive_int(max_pos) || std::stoi(max_pos) <= 0) {
            add_error(errors, "Risk", "MaxNetPosition", "must be a positive integer, got '" + max_pos + "'");
        }
    }

    const auto max_opm = config.get_string("Risk", "MaxOrdersPerMinute", "");
    if (!max_opm.empty()) {
        if (!is_positive_int(max_opm) || std::stoi(max_opm) <= 0) {
            add_error(errors, "Risk", "MaxOrdersPerMinute", "must be a positive integer, got '" + max_opm + "'");
        }
    }

    const auto cancel_rate = config.get_string("Risk", "MaxCancelRate", "");
    if (!cancel_rate.empty()) {
        if (!is_number(cancel_rate)) {
            add_error(errors, "Risk", "MaxCancelRate", "must be a number, got '" + cancel_rate + "'");
        } else {
            double val = std::stod(cancel_rate);
            if (val < 0.0 || val > 1.0) {
                add_error(errors, "Risk", "MaxCancelRate", "must be in [0, 1], got '" + cancel_rate + "'");
            }
        }
    }

    const auto daily_loss = config.get_string("Risk", "MaxDailyLoss", "");
    if (!daily_loss.empty()) {
        if (!is_number(daily_loss)) {
            add_error(errors, "Risk", "MaxDailyLoss", "must be a number, got '" + daily_loss + "'");
        } else if (std::stod(daily_loss) <= 0) {
            add_error(errors, "Risk", "MaxDailyLoss", "must be positive, got '" + daily_loss + "'");
        }
    }

    const auto window = config.get_string("Risk", "CancelRateWindowMinutes", "");
    if (!window.empty()) {
        if (!is_positive_int(window) || std::stoi(window) <= 0) {
            add_error(errors, "Risk", "CancelRateWindowMinutes",
                      "must be a positive integer, got '" + window + "'");
        }
    }
}

void ConfigValidator::check_strategies(const Config& config,
                                       std::vector<ConfigValidationError>& errors) const {
    const auto list_str = config.get_string("Strategies", "List", "");
    if (list_str.empty()) return;

    const auto strategy_ids = split_csv(list_str);
    const auto account_ids_str = config.get_string("Accounts", "List", "");
    const auto account_ids = split_csv(account_ids_str);
    std::set<std::string> account_set(account_ids.begin(), account_ids.end());

    for (const auto& sid : strategy_ids) {
        const std::string section = "Strategy." + sid;
        if (!config.has_section(section)) {
            add_error(errors, "Strategies", "List",
                      "strategy '" + sid + "' listed but [" + section + "] section missing");
            continue;
        }

        const auto type = config.get_string(section, "Type", "");
        if (!type.empty() && type != "python" && type != "simple") {
            add_error(errors, section, "Type",
                      "unsupported strategy type '" + type + "', expected 'python' or 'simple'");
        }

        if (type == "python") {
            if (config.get_string(section, "ScriptPath", "").empty()) {
                add_error(errors, section, "ScriptPath",
                          "ScriptPath is required when Type=python");
            }
        }

        const auto acct = config.get_string(section, "AccountID", "");
        if (!acct.empty() && !account_set.empty() && account_set.find(acct) == account_set.end()) {
            add_error(errors, section, "AccountID",
                      "AccountID '" + acct + "' is not in Accounts.List");
        }

        if (config.get_string(section, "Instruments", "").empty()) {
            add_error(errors, section, "Instruments", "Instruments list is empty");
        }

        const auto order_size = config.get_string(section, "OrderSize", "");
        if (!order_size.empty() && (!is_positive_int(order_size) || std::stoi(order_size) <= 0)) {
            add_error(errors, section, "OrderSize",
                      "must be a positive integer, got '" + order_size + "'");
        }
    }
}

void ConfigValidator::check_log(const Config& config,
                                std::vector<ConfigValidationError>& errors) const {
    const auto level = config.get_string("Log", "Level", "INFO");
    static const std::set<std::string> valid_levels = {"DEBUG", "INFO", "WARN", "ERROR"};
    if (valid_levels.find(level) == valid_levels.end()) {
        add_error(errors, "Log", "Level",
                  "must be one of {DEBUG, INFO, WARN, ERROR}, got '" + level + "'");
    }

    const auto queue_cap = config.get_string("Log", "QueueCapacity", "");
    if (!queue_cap.empty() && (!is_positive_int(queue_cap) || std::stoi(queue_cap) < 256)) {
        add_error(errors, "Log", "QueueCapacity",
                  "must be >= 256, got '" + queue_cap + "'");
    }

    const auto retention = config.get_string("Log", "RetentionDays", "");
    if (!retention.empty() && (!is_positive_int(retention) || std::stoi(retention) <= 0)) {
        add_error(errors, "Log", "RetentionDays",
                  "must be a positive integer, got '" + retention + "'");
    }
}

void ConfigValidator::check_web(const Config& config,
                                std::vector<ConfigValidationError>& errors) const {
    if (!config.has_section("Web")) return;

    const auto port_str = config.get_string("Web", "Port", "");
    if (!port_str.empty()) {
        if (!is_positive_int(port_str)) {
            add_error(errors, "Web", "Port", "must be a valid port number, got '" + port_str + "'");
        } else {
            int port = std::stoi(port_str);
            if (port < 1 || port > 65535) {
                add_error(errors, "Web", "Port",
                          "must be in [1, 65535], got '" + port_str + "'");
            }
        }
    }
}

void ConfigValidator::check_performance(const Config& config,
                                        std::vector<ConfigValidationError>& errors) const {
    if (!config.has_section("Performance")) return;

    const auto engine_core = config.get_string("Performance", "EngineCpuCore", "");
    if (!engine_core.empty() && !is_integer(engine_core)) {
        add_error(errors, "Performance", "EngineCpuCore",
                  "must be an integer (-1 to disable pinning), got '" + engine_core + "'");
    }

    const auto logger_core = config.get_string("Performance", "LoggerCpuCore", "");
    if (!logger_core.empty() && !is_integer(logger_core)) {
        add_error(errors, "Performance", "LoggerCpuCore",
                  "must be an integer (-1 to disable pinning), got '" + logger_core + "'");
    }

    const auto batch_size = config.get_string("Performance", "MdBatchSize", "");
    if (!batch_size.empty()) {
        if (!is_positive_int(batch_size)) {
            add_error(errors, "Performance", "MdBatchSize",
                      "must be a positive integer, got '" + batch_size + "'");
        } else {
            int val = std::stoi(batch_size);
            if (val < 1 || val > 65536) {
                add_error(errors, "Performance", "MdBatchSize",
                          "must be in [1, 65536], got '" + batch_size + "'");
            }
        }
    }
}

void ConfigValidator::check_runtime(const Config& config,
                                    std::vector<ConfigValidationError>& errors) const {
    if (!config.has_section("Runtime")) return;

    const auto mode = config.get_string("Runtime", "RunMode", "service");
    if (mode != "interactive" && mode != "service") {
        add_error(errors, "Runtime", "RunMode",
                  "must be 'interactive' or 'service', got '" + mode + "'");
    }

    const auto no_tick = config.get_string("Runtime", "NoTickWarnSeconds", "");
    if (!no_tick.empty() && (!is_positive_int(no_tick) || std::stoi(no_tick) <= 0)) {
        add_error(errors, "Runtime", "NoTickWarnSeconds",
                  "must be a positive integer, got '" + no_tick + "'");
    }
}

} // namespace hft
