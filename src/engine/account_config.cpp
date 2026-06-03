// ============================================
// account_config.cpp - Account configuration loading and saving implementation (账户配置加载与保存实现)
// ============================================

#include "engine/account_config.h"

#include "common/crypto.h"
#include "common/string_utils.h"
#include "strategy/strategy_config.h"

#include <algorithm>

namespace hft {

namespace {

AccountConfigSpec load_account_spec(const Config& config,
                                    const std::string& section,
                                    const std::string& account_id,
                                    const std::string& gateway_type) {
    AccountConfigSpec spec;
    spec.account_id = account_id;
    spec.gateway_type = gateway_type.empty() ? "CTP" : gateway_type;
    spec.broker_id = config.get_string(section, "BrokerID", "");
    spec.user_id = config.get_string(section, "UserID", "");
    spec.password = crypto::decrypt_config_value(config.get_string(section, "Password", ""));
    spec.app_id = config.get_string(section, "AppID", "");
    spec.auth_code = crypto::decrypt_config_value(config.get_string(section, "AuthCode", ""));
    spec.trade_front = config.get_string(section, "TradeFront", "");
    spec.market_front = config.get_string(section, "MarketFront", "");
    return spec;
}

} // namespace

AccountConfigBundle load_account_configs(const Config& config) {
    AccountConfigBundle bundle;

    if (config.has_section("Accounts")) {
        const auto account_ids = split_csv_trimmed(config.get_string("Accounts", "List", ""));
        bundle.market_data_account_id = config.get_string("Accounts", "MarketDataAccount", "");
        for (const auto& account_id : account_ids) {
            const std::string gateway_type = config.get_string("Accounts", account_id + ".Gateway", "CTP");
            const std::string section = gateway_type + "." + account_id;
            if (!config.has_section(section)) {
                continue;
            }
            bundle.accounts.push_back(load_account_spec(config, section, account_id, gateway_type));
        }
        if (bundle.market_data_account_id.empty() && !bundle.accounts.empty()) {
            bundle.market_data_account_id = bundle.accounts.front().account_id;
        }
        return bundle;
    }

    if (config.has_section("CTP")) {
        AccountConfigSpec spec = load_account_spec(config, "CTP", config.get_string("CTP", "UserID", ""), "CTP");
        if (spec.account_id.empty()) {
            spec.account_id = "default";
        }
        if (config.has_section("MD")) {
            spec.market_front = config.get_string("MD", "MarketFront", spec.market_front);
        }
        bundle.accounts.push_back(std::move(spec));
        bundle.market_data_account_id = bundle.accounts.front().account_id;
    }

    return bundle;
}

std::string resolve_market_data_config_section(const Config& config) {
    if (config.has_section("Accounts")) {
        const auto account_ids = split_csv_trimmed(config.get_string("Accounts", "List", ""));
        std::string market_data_account_id = trim_copy(config.get_string("Accounts", "MarketDataAccount", ""));
        if (market_data_account_id.empty() && !account_ids.empty()) {
            market_data_account_id = account_ids.front();
        }

        if (!market_data_account_id.empty()) {
            const std::string gateway_type = config.get_string(
                "Accounts", market_data_account_id + ".Gateway", "CTP");
            const std::string section = gateway_type + "." + market_data_account_id;
            if (config.has_section(section)) {
                return section;
            }
        }
    }

    if (config.has_section("MD")) {
        return "MD";
    }
    if (config.has_section("CTP")) {
        return "CTP";
    }
    return "";
}

std::string resolve_market_data_gateway_type(const Config& config) {
    if (config.has_section("Accounts")) {
        const auto account_ids = split_csv_trimmed(config.get_string("Accounts", "List", ""));
        std::string market_data_account_id = trim_copy(config.get_string("Accounts", "MarketDataAccount", ""));
        if (market_data_account_id.empty() && !account_ids.empty()) {
            market_data_account_id = account_ids.front();
        }
        if (!market_data_account_id.empty()) {
            return config.get_string("Accounts", market_data_account_id + ".Gateway", "CTP");
        }
    }
    return "CTP";
}

void save_account_configs(Config& config, const AccountConfigBundle& bundle) {
    config.erase_section("CTP");
    config.erase_section("MD");
    config.erase_section("Accounts");
    for (const auto& section : config.get_sections("CTP.")) {
        config.erase_section(section);
    }

    std::vector<std::string> account_ids;
    account_ids.reserve(bundle.accounts.size());
    for (const auto& spec : bundle.accounts) {
        account_ids.push_back(spec.account_id);
    }

    config.set_string("Accounts", "List", join_csv(account_ids));
    config.set_string("Accounts", "MarketDataAccount",
                      bundle.market_data_account_id.empty() && !bundle.accounts.empty()
                          ? bundle.accounts.front().account_id
                          : bundle.market_data_account_id);

    for (const auto& spec : bundle.accounts) {
        const std::string gateway_type = spec.gateway_type.empty() ? "CTP" : spec.gateway_type;
        const std::string section = gateway_type + "." + spec.account_id;

        config.set_string("Accounts", spec.account_id + ".Gateway", gateway_type);
        config.set_string(section, "BrokerID", spec.broker_id);
        config.set_string(section, "UserID", spec.user_id);
        config.set_string(section, "Password", crypto::encrypt_config_value(spec.password));
        config.set_string(section, "AppID", spec.app_id);
        config.set_string(section, "AuthCode", crypto::encrypt_config_value(spec.auth_code));
        config.set_string(section, "TradeFront", spec.trade_front);
        config.set_string(section, "MarketFront", spec.market_front);
    }

    config.erase_section("MD");
}

} // namespace hft
