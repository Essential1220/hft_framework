#pragma once
// ============================================
// i_trade_gateway.h - Trade gateway abstract interface (交易网关抽象接口)
//
// Defines the contract that all trade gateways (CTP, QDP, etc.) must fulfill.
// 定义所有交易网关（CTP、QDP 等）必须实现的接口契约。
// ============================================

#include "common/config.h"
#include "common/types.h"

#include <string>
#include <vector>

namespace hft {

class TradingEngine;

// Trade gateway interface: send orders, cancel orders, query account/position/orders
// 交易网关接口：发单（发送委托）、撤单（撤销委托）、查询资金/持仓/委托
class ITradeGateway {
public:
    virtual ~ITradeGateway() = default;

    // Initialize gateway with config, engine pointer, and account ID
    // 初始化网关：传入配置、引擎指针、资金账号 (初始化网关)
    virtual void init(const Config& config, const std::string& section,
                      TradingEngine* engine, const std::string& account_id) = 0;
    // Stop the gateway and release resources
    // 停止网关，释放资源 (停止网关)
    virtual void stop() = 0;
    // Block until login completes or timeout (wait for login / 等待登录完成)
    virtual bool wait_for_login(int timeout_sec = 30) = 0;
    virtual bool is_logged_in() const = 0;

    // Send an order request; returns 0 on success, non-zero on failure
    // 发送委托请求，成功返回0，失败返回非0 (发单)
    virtual int send_order(const OrderRequest& req, const std::string& order_ref) = 0;
    // Cancel an existing order by order_ref, front_id, and session_id
    // 撤销已有委托，通过 order_ref / front_id / session_id 定位 (撤单)
    virtual int cancel_order(const std::string& instrument_id, const std::string& exchange_id,
                             const std::string& order_ref, int front_id, int session_id) = 0;
    // Query account balance and margin (查询资金账户 / 查询资金)
    virtual int query_account() = 0;
    // Query positions; pass empty instrument_id for all positions (查询持仓 / 查询持仓)
    virtual int query_position(const std::string& instrument_id = "") = 0;
    // Query active (pending/partially-filled) orders (查询活动委托 / 查询活动委托)
    virtual int query_active_orders() = 0;
    // Query all available instruments from the exchange (查询合约列表 / 查询合约列表)
    virtual std::vector<std::string> query_instruments(int timeout_sec = 30) = 0;
    // Query margin rate and commission rate for a specific instrument
    // 查询指定合约的保证金率和手续费率 (查询保证金手续费率)
    virtual int query_instrument_rates(const std::string& instrument_id, const std::string& exchange_id = "") {
        (void)instrument_id;
        (void)exchange_id;
        return -1;
    }

    // Get front ID assigned by the broker on login (获取前置机编号 / 获取前置ID)
    virtual int get_front_id() const = 0;
    // Get session ID assigned by the broker on login (获取会话编号 / 获取会话ID)
    virtual int get_session_id() const = 0;
    // Get the maximum order reference value from the broker (获取最大报单引用 / 获取最大OrderRef)
    virtual int get_max_order_ref() const = 0;

    const std::string& get_account_id() const { return account_id_; }

protected:
    std::string account_id_; // Associated account identifier (关联的资金账号)
};

} // namespace hft