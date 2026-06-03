// ============================================
// account_manager.cpp - Multi-account manager implementation (多账户管理器实现)
// Handles account discovery, gateway factory dispatch, and account routing.
// Supports single-account compatibility mode and multi-account mode.
// (负责账户发现、网关工厂分发、账户路由。支持单账户兼容模式和多账户模式)
// ============================================

#include "engine/account_manager.h"

#include "common/logger.h"
#include "engine/trading_engine.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace hft {

// Register trade gateway factory function, used to create corresponding gateway instances by type.
// (注册交易网关工厂函数, 用于根据类型创建对应的网关实例)
void AccountManager::register_gateway_factory(TradeGatewayFactory factory) {
    gateway_factory_ = std::move(factory);
}

// Initialize multi-account manager: read account info from config and create gateways.
// (初始化多账户管理器, 从配置中读取账户信息并创建网关)
bool AccountManager::init(const Config& config, TradingEngine* engine) {
    (void)engine; // Engine parameter currently unused (当前未使用 engine 参数)

    accounts_.clear();
    account_index_.clear();

    // ---- Detect configuration mode (探测配置模式) ----
    // Mode 1: multi-account config [Accounts] List = Account1, Account2
    // Mode 2: single-account compatibility mode, use [CTP] directly
    // (模式 1: 多账户配置 [Accounts] List = Account1, Account2
    //  模式 2: 单账户兼容模式, 直接使用 [CTP])
    if (config.has_section("Accounts")) {
        std::string list_str = config.get_string("Accounts", "List", "");
        if (list_str.empty()) {
            LOG_ERROR("账户管理器: [Accounts] List 为空");
            return false;
        }

        std::istringstream iss(list_str);
        std::string token;
        // Parse comma-separated account list (解析逗号分隔的账户列表)
        while (std::getline(iss, token, ',')) {
            token.erase(0, token.find_first_not_of(" \t")); // Strip leading spaces (去除前导空格)
            token.erase(token.find_last_not_of(" \t") + 1); // Strip trailing spaces (去除尾部空格)
            if (token.empty()) {
                continue;
            }

            // 约定配置节格式为 "<GatewayType>.<AccountID>"。
            // 也支持在 [Accounts] 段内通过 <AccountID>.Gateway 覆盖网关类型。
            // Convention: config section format is "<GatewayType>.<AccountID>".
            // Also supports overriding gateway type via <AccountID>.Gateway in [Accounts] section.
            std::string gateway_type = config.get_string("Accounts", token + ".Gateway", "CTP");
            std::string section = gateway_type + "." + token;

            if (!config.has_section(section)) {
                LOG_ERROR("账户管理器: 配置节 [" + section + "] 不存在");
                return false;
            }

            // Create account context and initialize basic info (创建账户上下文并初始化基本信息)
            auto ctx = std::make_unique<AccountContext>();
            ctx->account_id = token;
            ctx->config_section = section;
            ctx->gateway_type = gateway_type;

            LOG_INFO("账户管理器: 发现账户 " + token +
                     " gateway=" + gateway_type +
                     " section=[" + section + "]");

            account_index_[token] = ctx.get(); // Add to index map (添加到索引映射)
            accounts_.push_back(std::move(ctx)); // Add to account list (添加到账户列表)
        }
    } else if (config.has_section("CTP")) {
        // Single-account mode handling (单账户模式处理)
        auto ctx = std::make_unique<AccountContext>();
        ctx->account_id = config.get_string("CTP", "UserID");
        ctx->config_section = "CTP";
        ctx->gateway_type = "CTP";

        LOG_INFO("账户管理器: 单账户兼容模式，使用 [CTP] 配置");

        account_index_[""] = ctx.get(); // Empty string also maps to default account (空字符串也映射到默认账户)
        if (!ctx->account_id.empty()) {
            account_index_[ctx->account_id] = ctx.get();
        }
        accounts_.push_back(std::move(ctx));
    } else {
        LOG_ERROR("账户管理器: 未找到 [Accounts] 或 [CTP] 配置节");
        return false;
    }

    if (accounts_.empty()) {
        LOG_ERROR("账户管理器: 未发现任何账户配置");
        return false;
    }

    // Create trade gateway for each account based on parsed config (根据解析出的配置创建各个账户的交易网关)
    for (auto& ctx : accounts_) {
        if (!gateway_factory_) {
            LOG_ERROR("账户管理器: 未注册网关工厂");
            return false;
        }

        ctx->trade_gateway = gateway_factory_(ctx->gateway_type);
        if (!ctx->trade_gateway) {
            LOG_ERROR("账户管理器: 无法创建网关类型=" + ctx->gateway_type +
                      " 账户=" + ctx->account_id);
            return false;
        }
    }

    LOG_INFO("账户管理器: 初始化完成，账户数=" + std::to_string(accounts_.size()));
    return true;
}

// Start all account trade gateways — on partial failure, continue trying remaining accounts and return summary result.
// (启动所有账户的交易网关, 部分失败时继续尝试其他账户, 返回汇总结果)
AccountManager::StartAllResult AccountManager::start_all(const Config& config, TradingEngine* engine) {
    StartAllResult result;
    for (auto& ctx : accounts_) {
        constexpr int kLoginAttempts = 3;
        bool login_ok = false;
        for (int attempt = 1; attempt <= kLoginAttempts; ++attempt) {
            ctx->trade_gateway->stop();
            ctx->trade_gateway->init(config, ctx->config_section, engine, ctx->account_id);

            if (ctx->trade_gateway->wait_for_login(30)) {
                login_ok = true;
                break;
            }

            LOG_WARN("账户管理器: 交易网关登录超时，账户=" + ctx->account_id +
                     " attempt=" + std::to_string(attempt) + "/" + std::to_string(kLoginAttempts));
            ctx->trade_gateway->stop();
            if (attempt < kLoginAttempts) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }
        if (!login_ok) {
            LOG_ERROR("账户管理器: 交易网关登录失败，账户=" + ctx->account_id);
            result.all_ok = false;
            result.failed_accounts.push_back(ctx->account_id);
            continue; // Don't return false; keep trying other accounts (不再 return false, 继续尝试其他账户)
        }

        // After successful login, initialize the account's order manager (登录成功后初始化该账户的委托管理器)
        ctx->order_mgr.init(
            ctx->trade_gateway->get_front_id(),
            ctx->trade_gateway->get_session_id(),
            ctx->trade_gateway->get_max_order_ref());

        result.ok_accounts.push_back(ctx->account_id);
        LOG_INFO("账户管理器: 账户 " + ctx->account_id + " 交易网关登录成功");
    }

    return result;
}

// Stop all account trade gateways.
// (停止所有账户的交易网关)
void AccountManager::stop_all() {
    for (auto& ctx : accounts_) {
        if (ctx->trade_gateway) {
            ctx->trade_gateway->stop();
        }
    }
    LOG_INFO("账户管理器: 所有交易网关已停止");
}

// Find the context for a specific account.
// (查找指定账户的上下文)
AccountContext* AccountManager::find_account(const std::string& account_id) {
    if (account_id.empty()) {
        return default_account(); // Empty string returns default account (空字符串返回默认账户)
    }
    auto it = account_index_.find(account_id);
    return (it != account_index_.end()) ? it->second : nullptr;
}

// Find the context for a specific account (const version).
// (查找指定账户的上下文, const 版本)
const AccountContext* AccountManager::find_account(const std::string& account_id) const {
    if (account_id.empty()) {
        return default_account(); // Empty string returns default account (空字符串返回默认账户)
    }
    auto it = account_index_.find(account_id);
    return (it != account_index_.end()) ? it->second : nullptr;
}

// Get the default account (first account in the list).
// (获取默认账户, 列表中的第一个账户)
AccountContext* AccountManager::default_account() {
    return accounts_.empty() ? nullptr : accounts_.front().get();
}

// Get the default account (const version).
// (获取默认账户, const 版本)
const AccountContext* AccountManager::default_account() const {
    return accounts_.empty() ? nullptr : accounts_.front().get();
}

// Get list of all accounts.
// (获取所有账户的列表)
std::vector<AccountContext*> AccountManager::all_accounts() {
    std::vector<AccountContext*> result;
    result.reserve(accounts_.size());
    for (auto& ctx : accounts_) {
        result.push_back(ctx.get());
    }
    return result;
}

// Get list of all accounts (const version).
// (获取所有账户的列表, const 版本)
std::vector<const AccountContext*> AccountManager::all_accounts() const {
    std::vector<const AccountContext*> result;
    result.reserve(accounts_.size());
    for (const auto& ctx : accounts_) {
        result.push_back(ctx.get());
    }
    return result;
}

// Get merged positions, or positions for a specific account.
// (获取合并的持仓信息, 或指定账户的持仓信息)
std::vector<PositionInfo> AccountManager::get_all_positions(const std::string& account_id) const {
    std::vector<PositionInfo> result;
    if (!account_id.empty()) {
        const AccountContext* ctx = find_account(account_id);
        if (!ctx) {
            return result;
        }
        return ctx->position_mgr.get_all_positions();
    }
    // No account specified, merge all accounts' positions (未指定账户则合并所有账户的持仓)
    for (const auto& ctx : accounts_) {
        auto positions = ctx->position_mgr.get_all_positions();
        result.insert(result.end(), positions.begin(), positions.end());
    }
    return result;
}

// Get merged active orders, or active orders for a specific account.
// (获取合并的活动委托, 或指定账户的活动委托)
std::vector<OrderInfo> AccountManager::get_all_active_orders(const std::string& account_id) const {
    std::vector<OrderInfo> result;
    if (!account_id.empty()) {
        const AccountContext* ctx = find_account(account_id);
        if (!ctx) {
            return result;
        }
        return ctx->order_mgr.get_active_orders();
    }
    // No account specified, merge all accounts' active orders (未指定账户则合并所有账户的活动委托)
    for (const auto& ctx : accounts_) {
        auto orders = ctx->order_mgr.get_active_orders();
        result.insert(result.end(), orders.begin(), orders.end());
    }
    return result;
}

// Get merged account fund info, or funds for a specific account.
// (获取合并的账户资金信息, 或指定账户的资金信息)
AccountInfo AccountManager::get_account(const std::string& account_id) const {
    if (!account_id.empty()) {
        const AccountContext* ctx = find_account(account_id);
        if (!ctx) {
            return AccountInfo{};
        }
        std::lock_guard<std::mutex> lock(ctx->account_mtx);
        return ctx->account_info;
    }

    // No account specified, aggregate all accounts' funds (未指定账户则聚合所有账户的资金)
    AccountInfo agg{};
    for (const auto& ctx : accounts_) {
        std::lock_guard<std::mutex> lock(ctx->account_mtx);
        agg.balance += ctx->account_info.balance;
        agg.available += ctx->account_info.available;
        agg.margin += ctx->account_info.margin;
        agg.commission += ctx->account_info.commission;
        agg.close_profit += ctx->account_info.close_profit;
        agg.position_profit += ctx->account_info.position_profit;
        agg.frozen_margin += ctx->account_info.frozen_margin;
        agg.frozen_commission += ctx->account_info.frozen_commission;
    }
    safe_copy(agg.account_id, "aggregated", sizeof(agg.account_id));
    return agg;
}

// Route the request to the corresponding account context based on account_id in the order request.
// (根据订单请求中的 account_id 将请求路由到对应的账户上下文)
AccountContext* AccountManager::route_order(const OrderRequest& req) {
    std::string target(req.account_id);
    return find_account(target);
}

} // namespace hft
