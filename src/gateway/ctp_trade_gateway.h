#pragma once
// ============================================
// ctp_trade_gateway.h - CTP trade gateway (CTP 交易网关)
//
// Implements ITradeGateway using CTP's ThostFtdcTraderApi.
// Handles authentication, login, order insertion, cancellation, and query.
// 基于 CTP ThostFtdcTraderApi 实现交易网关接口，
// 处理认证、登录、发单（报单插入）、撤单（撤销委托）和查询。
// ============================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ThostFtdcTraderApi.h"
#include "common/config.h"
#include "common/types.h"
#include "gateway/i_trade_gateway.h"

namespace hft {

class TradingEngine;

class CtpTradeGateway : public ITradeGateway, public CThostFtdcTraderSpi {
public:
    CtpTradeGateway() = default;
    ~CtpTradeGateway();

    void init(const Config& config, const std::string& section,
              TradingEngine* engine, const std::string& account_id) override;
    void stop() override;
    bool wait_for_login(int timeout_sec = 30) override;
    bool is_logged_in() const override { return logged_in_; }

    int send_order(const OrderRequest& req, const std::string& order_ref) override;
    int cancel_order(const std::string& instrument_id, const std::string& exchange_id,
                     const std::string& order_ref, int front_id, int session_id) override;
    int query_account() override;
    int query_position(const std::string& instrument_id = "") override;
    int query_active_orders() override;
    std::vector<std::string> query_instruments(int timeout_sec = 30) override;
    int query_instrument_rates(const std::string& instrument_id, const std::string& exchange_id = "") override;

    int get_front_id() const override { return front_id_; }
    int get_session_id() const override { return session_id_; }
    int get_max_order_ref() const override { return max_order_ref_; }

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                           CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                    CThostFtdcRspInfoField* pRspInfo,
                                    int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                          CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                          CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                CThostFtdcRspInfoField* pRspInfo,
                                int nRequestID, bool bIsLast) override;
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                  CThostFtdcRspInfoField* pRspInfo,
                                  int nRequestID, bool bIsLast) override;
    void OnRspQryOrder(CThostFtdcOrderField* pOrder,
                       CThostFtdcRspInfoField* pRspInfo,
                       int nRequestID, bool bIsLast) override;
    void OnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField* pInstrumentMarginRate,
                                      CThostFtdcRspInfoField* pRspInfo,
                                      int nRequestID, bool bIsLast) override;
    void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField* pInstrumentCommissionRate,
                                          CThostFtdcRspInfoField* pRspInfo,
                                          int nRequestID, bool bIsLast) override;
    void OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument,
                            CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

private:
    struct PendingPositionSnapshot {
        PositionInfo pos{};
        double total_cost = 0.0;
    };

    void req_authenticate();
    void req_login();
    void req_settlement_confirm();
    void report_gateway_error(OrderRejectReason reason, const std::string& message);
    bool is_error_rsp(CThostFtdcRspInfoField* pRspInfo);

    // CTP 查询限流器：CTP 柜台每秒只允许 1 次查询请求，
    // 连续快速查询会返回 -3 (FlowControl)。所有 query_* 方法
    // 在发出请求前必须调用此函数等待。
    void throttle_query();

    CThostFtdcTraderApi* api_ = nullptr;     // CTP trade API instance (CTP 交易 API 实例)
    TradingEngine* engine_ = nullptr;        // Trading engine pointer for event dispatch (交易引擎指针)

    std::string broker_id_;                 // Broker ID (经纪公司代码)
    std::string user_id_;                   // Investor ID (投资者代码)
    std::string password_;                  // Login password (密码)
    std::string app_id_;                    // Application ID for CTP auth (CTP 认证 AppID)
    std::string auth_code_;                 // Auth code for CTP auth (CTP 认证授权码)
    std::atomic<int> request_id_{0};        // Monotonic request ID counter (请求编号递增计数器)
    int front_id_ = 0;                      // Front ID assigned by CTP on login (CTP 登录分配的前置编号)
    int session_id_ = 0;                    // Session ID assigned by CTP on login (CTP 登录分配的会话编号)
    int max_order_ref_ = 0;                 // Max order ref from CTP login response (CTP 返回的最大报单引用)
    bool initial_login_done_ = false;       // Whether initial login is complete (首次登录是否已完成)
    int pending_positions_request_id_ = 0;  // Request ID for pending position query (持仓查询请求ID)
    int pending_active_orders_request_id_ = 0; // Request ID for pending active orders query (活动委托查询请求ID)
    int pending_instruments_request_id_ = 0;   // Request ID for pending instruments query (合约列表查询请求ID)

    std::map<std::string, PendingPositionSnapshot> pending_positions_; // Aggregated position query results (聚合持仓查询结果)
    std::vector<OrderInfo> pending_active_orders_; // Active orders collected during query (查询中收集的活动委托)

    std::mutex pending_positions_mtx_;       // Mutex for pending position aggregation (持仓聚合锁)
    std::mutex pending_orders_mtx_;          // Mutex for pending orders collection (委托收集锁)
    std::mutex pending_instruments_mtx_;     // Mutex for pending instruments collection (合约列表收集锁)
    std::condition_variable pending_instruments_cv_; // CV to signal instruments query completion (合约查询完成信号)
    std::vector<std::string> pending_instruments_;   // Instruments collected during query (查询中收集的合约列表)
    bool pending_instruments_done_ = false;  // Whether instruments query is complete (合约查询是否完成)

    std::atomic<bool> logged_in_{false};     // Login status flag (登录状态标志)
    std::atomic<bool> login_failed_{false};  // Login failure flag (登录失败标志)
    std::mutex login_mtx_;                   // Mutex for login synchronization (登录同步锁)
    std::condition_variable login_cv_;       // CV to signal login completion (登录完成条件变量)

    // CTP query rate limiter: CTP allows at most 1 query per second;
    // CTP 查询限流：记录上次查询发出的时间点，串行化查询请求
    // records the last query timestamp and serializes query requests.
    std::mutex query_throttle_mtx_;
    std::chrono::steady_clock::time_point last_query_time_{};
    static constexpr int kQueryIntervalMs = 1100; // CTP requires interval >= 1s; 100ms safety margin (CTP 要求间隔 ≥1s，留 100ms 余量)
};

} // namespace hft
