#pragma once
// ============================================
// account_config.h - Account configuration loading and saving (账户配置的加载与保存)
// ============================================

#include "common/config.h"

#include <string>
#include <vector>

namespace hft {

struct AccountConfigSpec {
    std::string account_id;
    std::string gateway_type = "CTP";
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string app_id;
    std::string auth_code;
    std::string trade_front;
    std::string market_front;
};

struct AccountConfigBundle {
    std::string market_data_account_id;
    std::vector<AccountConfigSpec> accounts;
};

AccountConfigBundle load_account_configs(const Config& config);
std::string resolve_market_data_config_section(const Config& config);
// Resolve MarketDataAccount's gateway type (解析行情账户的网关类型)
// Returns gateway type (CTP / QDP / ...), empty string defaults to CTP. (返回 gateway_type, 空串则默认 CTP)
std::string resolve_market_data_gateway_type(const Config& config);
void save_account_configs(Config& config, const AccountConfigBundle& bundle);

} // namespace hft
