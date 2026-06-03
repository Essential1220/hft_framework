#pragma once
// ============================================
// qdp_md_gateway.h — QDP(上期所 FTD)行情网关
// 协议与 CTP 类同源,接口面相同,差别仅在命名空间(CThostFtdc -> CFtdc)和
// SDK header 名(ThostFtdcMdApi.h -> FtdcMdApi.h)。
//
// 仅在 HFT_ENABLE_QDP=ON 编译时生效;OFF 时整个 TU 是空文件,
// 不需要 QDP SDK 即可保留源码于仓库。
// ============================================

#ifdef HFT_HAS_QDP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/types.h"
#include "gateway/i_md_gateway.h"
#include "FtdcMdApi.h"

namespace hft {

class TradingEngine;

class QdpMdGateway : public IMdGateway, public CFtdcMdSpi {
public:
    QdpMdGateway() = default;
    ~QdpMdGateway();

    // ---- IMdGateway ----
    void init(const Config& config, const std::string& section, TradingEngine* engine) override;
    void subscribe(const std::vector<std::string>& instruments) override;
    void subscribe_append(const std::vector<std::string>& instruments) override;
    void unsubscribe(const std::vector<std::string>& instruments) override;
    void stop() override;
    bool is_logged_in() const override { return logged_in_; }
    bool wait_for_login(int timeout_sec = 10) override;
    MdGatewayStatus status() const override { return status_.load(std::memory_order_acquire); }

    // ---- CFtdcMdSpi 回调 ----
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CFtdcRspUserLoginField* pRspUserLogin,
                        CFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspSubMarketData(CFtdcSpecificInstrumentField* pSpecificInstrument,
                            CFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(CFtdcDepthMarketDataField* pDepthMarketData) override;
    void OnRspError(CFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

private:
    bool is_error_rsp(CFtdcRspInfoField* pRspInfo);

    CFtdcMdApi* api_ = nullptr;
    TradingEngine* engine_ = nullptr;
    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    std::string front_;
    int request_id_ = 0;
    std::vector<std::string> subscribed_instruments_;
    std::atomic<int> subscription_success_count_{0};
    std::atomic<int> subscription_error_count_{0};

    std::atomic<bool> logged_in_{false};
    std::atomic<MdGatewayStatus> status_{MdGatewayStatus::Disconnected};
    std::mutex login_mtx_;
    std::condition_variable login_cv_;
};

} // namespace hft

#endif // HFT_HAS_QDP
