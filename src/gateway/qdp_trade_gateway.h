#pragma once
// ============================================
// qdp_trade_gateway.h — QDP(上期所 FTD)交易网关
// 与 CTP 接口面一致,差异:
//   1) SDK 头 ThostFtdcTraderApi.h -> FtdcTraderApi.h
//   2) 类前缀 CThostFtdc -> CFtdc
//   3) QDP 无 AppID / AuthCode,登录链路是
//        OnFrontConnected -> ReqUserLogin -> ReqSettlementInfoConfirm
//      跳过 CTP 中的 ReqAuthenticate 一步
// 仅在 HFT_ENABLE_QDP=ON 编译时生效。
// ============================================

#ifdef HFT_HAS_QDP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FtdcTraderApi.h"
#include "common/config.h"
#include "common/types.h"
#include "gateway/i_trade_gateway.h"

namespace hft {

class TradingEngine;

class QdpTradeGateway : public ITradeGateway, public CFtdcTraderSpi {
public:
    QdpTradeGateway() = default;
    ~QdpTradeGateway();

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
    void OnRspUserLogin(CFtdcRspUserLoginField* pRspUserLogin,
                        CFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspSettlementInfoConfirm(CFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                    CFtdcRspInfoField* pRspInfo,
                                    int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CFtdcInputOrderField* pInputOrder,
                          CFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRspOrderAction(CFtdcInputOrderActionField* pInputOrderAction,
                          CFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRtnOrder(CFtdcOrderField* pOrder) override;
    void OnRtnTrade(CFtdcTradeField* pTrade) override;
    void OnRspQryTradingAccount(CFtdcTradingAccountField* pTradingAccount,
                                CFtdcRspInfoField* pRspInfo,
                                int nRequestID, bool bIsLast) override;
    void OnRspQryInvestorPosition(CFtdcInvestorPositionField* pInvestorPosition,
                                  CFtdcRspInfoField* pRspInfo,
                                  int nRequestID, bool bIsLast) override;
    void OnRspQryOrder(CFtdcOrderField* pOrder,
                       CFtdcRspInfoField* pRspInfo,
                       int nRequestID, bool bIsLast) override;
    void OnRspQryInstrumentMarginRate(CFtdcInstrumentMarginRateField* pInstrumentMarginRate,
                                      CFtdcRspInfoField* pRspInfo,
                                      int nRequestID, bool bIsLast) override;
    void OnRspQryInstrumentCommissionRate(CFtdcInstrumentCommissionRateField* pInstrumentCommissionRate,
                                          CFtdcRspInfoField* pRspInfo,
                                          int nRequestID, bool bIsLast) override;
    void OnRspQryInstrument(CFtdcInstrumentField* pInstrument,
                            CFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;
    void OnRspError(CFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

private:
    struct PendingPositionSnapshot {
        PositionInfo pos{};
        double total_cost = 0.0;
    };

    void req_login();
    void req_settlement_confirm();
    void report_gateway_error(OrderRejectReason reason, const std::string& message);
    bool is_error_rsp(CFtdcRspInfoField* pRspInfo);
    void throttle_query();

    CFtdcTraderApi* api_ = nullptr;
    TradingEngine* engine_ = nullptr;

    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    // Note: QDP has no AppID/AuthCode, so two fewer members than CtpTradeGateway.
    // 注意：QDP 没有 AppID/AuthCode，这里相比 CtpTradeGateway 少两个成员。 (QDP成员差异)
    std::atomic<int> request_id_{0};
    int front_id_ = 0;
    int session_id_ = 0;
    int max_order_ref_ = 0;
    bool initial_login_done_ = false;
    int pending_positions_request_id_ = 0;
    int pending_active_orders_request_id_ = 0;
    int pending_instruments_request_id_ = 0;

    std::map<std::string, PendingPositionSnapshot> pending_positions_;
    std::vector<OrderInfo> pending_active_orders_;

    std::mutex pending_positions_mtx_;
    std::mutex pending_orders_mtx_;
    std::mutex pending_instruments_mtx_;
    std::condition_variable pending_instruments_cv_;
    std::vector<std::string> pending_instruments_;
    bool pending_instruments_done_ = false;

    std::atomic<bool> logged_in_{false};
    std::atomic<bool> login_failed_{false};
    std::mutex login_mtx_;
    std::condition_variable login_cv_;

    std::mutex query_throttle_mtx_;
    std::chrono::steady_clock::time_point last_query_time_{};
    static constexpr int kQueryIntervalMs = 1100;
};

} // namespace hft

#endif // HFT_HAS_QDP
