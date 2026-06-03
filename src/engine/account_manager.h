#pragma once
// ============================================
// account_manager.h - Multi-account manager (多账户管理器)
// Handles account discovery, gateway factory dispatch, and account routing.
// Supports single-account compatibility mode and multi-account mode.
// (负责账户发现、网关工厂分发、账户路由。支持单账户兼容模式和多账户模式)
//
// Config format (配置格式):
//   Single-account (compat, 单账户兼容旧版):
//     [CTP]
//     BrokerID = ...
//
//   Multi-account (多账户):
//     [Accounts]
//     List = Account1, Account2
//
//     [CTP.Account1]
//     BrokerID = ...
//
//     [CTP.Account2]
//     BrokerID = ...
// ============================================

#include "common/config.h"
#include "engine/account_context.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hft {

class TradingEngine;

// 网关工厂函数类型：给定网关类型名称（如 "CTP"），返回 ITradeGateway 实例
// Gateway factory function type: given a gateway type name (e.g. "CTP"), returns an ITradeGateway instance.
using TradeGatewayFactory = std::function<std::unique_ptr<ITradeGateway>(const std::string& gateway_type)>;

class AccountManager {
public:
    AccountManager() = default;

    // 注册网关工厂，在 init 之前调用。用于根据网关类型创建不同的网关实例。
    // Register gateway factory, called before init. Used to create different gateway instances by type.
    void register_gateway_factory(TradeGatewayFactory factory);

    // 初始化方法。从配置中发现并创建所有账户的上下文环境。
    // Initialization. Discover and create context environments for all accounts from configuration.
    bool init(const Config& config, TradingEngine* engine);

    // start_all 返回结果：部分账户登录失败时，引擎可继续以 MD-only 模式运行
    // start_all return result: when some accounts fail to log in, engine can continue in MD-only mode.
    struct StartAllResult {
        bool all_ok = true;
        std::vector<std::string> failed_accounts;
        std::vector<std::string> ok_accounts;
    };

    // 启动所有账户。初始化并登录所有账户绑定的交易网关。
    // 即使部分账户失败，也会返回结果而非直接终止——由调用方决定是否降级。
    // Start all accounts. Initialize and log in all account-bound trade gateways.
    // Even if some accounts fail, returns result instead of terminating — caller decides whether to degrade.
    StartAllResult start_all(const Config& config, TradingEngine* engine);

    // 停止所有账户网关的运行。
    // Stop all account gateways.
    void stop_all();

    // ---- 账户查找 / Account Lookup ----

    // 根据 account_id 查找对应的 AccountContext（若传入空串，则返回默认账户）
    // Find AccountContext by account_id (returns default account if empty string is passed)
    AccountContext* find_account(const std::string& account_id);
    const AccountContext* find_account(const std::string& account_id) const;

    // 获取默认账户（即配置中定义的第一个账户，如果是单账户模式则为唯一的那个）
    // Get default account (first account defined in config, or the only one in single-account mode)
    AccountContext* default_account();
    const AccountContext* default_account() const;

    // 获取所有加载的账户列表
    // Get list of all loaded accounts
    std::vector<AccountContext*> all_accounts();
    std::vector<const AccountContext*> all_accounts() const;

    // 返回当前管理的账户总数
    // Return total count of managed accounts
    size_t account_count() const { return accounts_.size(); }

    // ---- 聚合查询（跨账户）/ Aggregated Queries (Cross-Account) ----

    // 获取指定账户的持仓；如果未指定账户（空字符串），则合并所有账户的持仓返回
    // Get positions for a specific account; if empty string, merge and return all accounts' positions
    std::vector<PositionInfo> get_all_positions(const std::string& account_id = "") const;

    // 获取指定账户的活跃委托；如果未指定，则合并所有账户的活跃委托返回
    // Get active orders for a specific account; if unspecified, merge all accounts' active orders
    std::vector<OrderInfo> get_all_active_orders(const std::string& account_id = "") const;

    // 获取指定账户的资金；如果未指定，则将所有账户的资金简单相加后返回
    // Get account funds; if unspecified, sum all accounts' funds
    AccountInfo get_account(const std::string& account_id = "") const;

    // ---- 路由下单 / Order Routing ----

    // 根据 OrderRequest 中的 account_id，路由并返回负责处理该请求的 AccountContext
    // Route by OrderRequest.account_id and return the AccountContext responsible for it
    AccountContext* route_order(const OrderRequest& req);

private:
    // 账户列表。保持有序，第一个元素作为默认账户
    // Account list. Kept in order; first element serves as default account.
    std::vector<std::unique_ptr<AccountContext>> accounts_;

    // 账户索引。键为 account_id，值为指向 AccountContext 的指针，用于快速查找
    // Account index. Keyed by account_id, stores raw pointer to AccountContext for fast lookup.
    std::map<std::string, AccountContext*> account_index_;

    // 网关创建工厂，由外部在初始化前注入
    // Gateway creation factory, injected externally before initialization.
    TradeGatewayFactory gateway_factory_;
};

} // namespace hft
