#pragma once
// ============================================
// ctp_md_gateway.h - CTP 行情网关
// 处理连接、登录、订阅和 Tick 推送
// ============================================

#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>

#include "common/types.h"
#include "common/config.h"
#include "gateway/i_md_gateway.h"
#include "ThostFtdcMdApi.h"

namespace hft {

class TradingEngine;

class CtpMdGateway : public IMdGateway, public CThostFtdcMdSpi {
public:
    CtpMdGateway() = default;
    ~CtpMdGateway();

    // ---- IMdGateway 接口实现 ----
    void init(const Config& config, const std::string& section, TradingEngine* engine) override;
    void subscribe(const std::vector<std::string>& instruments) override;
    void subscribe_append(const std::vector<std::string>& instruments) override;
    void unsubscribe(const std::vector<std::string>& instruments) override;
    void stop() override;
    bool is_logged_in() const override { return logged_in_; }
    bool wait_for_login(int timeout_sec = 10) override;
    MdGatewayStatus status() const override { return status_.load(std::memory_order_acquire); }

    // ---- CThostFtdcMdSpi callback implementations (CTP 行情回调实现) ----
    // Called when the client establishes a connection with the trading backend (before login)
    // 当客户端与交易后台建立起通信连接时（还未登录前），该方法被调用 (前置连接)
    void OnFrontConnected() override;
    // Called when the client disconnects from the trading backend
    // 当客户端与交易后台通信连接断开时，该方法被调用 (前置断开)
    void OnFrontDisconnected(int nReason) override;
    // Login response (登录请求响应 / 登录响应)
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    // Market data subscription response (订阅行情应答 / 订阅行情响应)
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                            CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;
    // Depth market data notification (深度行情通知 / 行情快照回调)
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override;
    // Error response (错误应答 / 错误响应)
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

private:
    // Check if the response indicates an error (检查是否为错误响应 / 错误检查)
    bool is_error_rsp(CThostFtdcRspInfoField* pRspInfo);

    CThostFtdcMdApi* api_ = nullptr;        // CTP market data API instance (CTP 行情 API 实例)
    TradingEngine* engine_ = nullptr;       // Trading engine pointer for event dispatch (交易引擎指针，用于推送事件)
    std::string broker_id_;                 // Broker ID (经纪公司代码)
    std::string user_id_;                   // Investor ID (投资者代码)
    std::string password_;                  // Password (密码)
    std::string front_;                     // Current market data front address (当前行情前置地址)
    int request_id_ = 0;                    // Monotonic request ID (请求编号)
    std::vector<std::string> subscribed_instruments_; // List of subscribed instruments (已订阅的合约列表)
    std::atomic<int> subscription_success_count_{0};  // Count of successful subscriptions (订阅成功计数)
    std::atomic<int> subscription_error_count_{0};    // Count of failed subscriptions (订阅失败计数)

    std::atomic<bool> logged_in_{false};    // Login status flag (是否已登录)
    std::atomic<MdGatewayStatus> status_{MdGatewayStatus::Disconnected}; // Current connection status (当前连接状态)
    std::mutex login_mtx_;                  // Mutex for login synchronization (登录同步互斥锁)
    std::condition_variable login_cv_;      // CV for login completion signal (登录同步条件变量)
};

} // namespace hft
