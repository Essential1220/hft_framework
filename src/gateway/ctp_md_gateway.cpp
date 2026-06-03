#include "gateway/ctp_md_gateway.h"

#include "common/crypto.h"
#include "common/encoding.h"
#include "common/logger.h"
#include "engine/trading_engine.h"

#include <cmath>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <thread>

namespace hft {

namespace {

// CTP uses DBL_MAX-like sentinel values when a price is unavailable.
// CTP 在价格无效时使用类似 DBL_MAX 的哨兵值 (价格清洗)
inline double sanitize_price(double price) {
    if (price >= DBL_MAX * 0.99 || price <= -DBL_MAX * 0.99 || std::isnan(price)) {
        return 0.0;
    }
    return price;
}

std::string summarize_instruments(const std::vector<std::string>& instruments, size_t preview_limit = 16) {
    std::string text = "count=" + std::to_string(instruments.size());
    const size_t preview_count = std::min(instruments.size(), preview_limit);
    if (preview_count == 0) return text;

    text += " preview=";
    for (size_t i = 0; i < preview_count; ++i) {
        if (i > 0) text += " ";
        text += instruments[i];
    }
    if (instruments.size() > preview_count) {
        text += " ...";
    }
    return text;
}

} // namespace

CtpMdGateway::~CtpMdGateway() {
    stop();
}

void CtpMdGateway::init(const Config& config, const std::string& section, TradingEngine* engine) {
    engine_ = engine;
    broker_id_ = config.get_string(section, "BrokerID");
    user_id_ = config.get_string(section, "UserID");
    password_ = crypto::decrypt_config_value(config.get_string(section, "Password"));
    front_ = config.get_string(section, "MarketFront");
    if (front_.empty()) {
        LOG_ERROR("行情网关前置地址为空, section=" + section);
        return;
    }

    std::string flow_dir = "md_flow_" + section + "//";
    std::filesystem::create_directories("md_flow_" + section);

    api_ = CThostFtdcMdApi::CreateFtdcMdApi(flow_dir.c_str());
    if (!api_) {
        LOG_ERROR("行情网关创建API失败, section=" + section);
        return;
    }
    api_->RegisterSpi(this);
    api_->RegisterFront(const_cast<char*>(front_.c_str()));

    LOG_INFO("market data gateway connecting front: " + front_);
    status_.store(MdGatewayStatus::Connecting, std::memory_order_release);
    api_->Init();
}

void CtpMdGateway::subscribe(const std::vector<std::string>& instruments) {
    if (instruments.empty()) return;
    if (!api_) {
        LOG_ERROR("market data subscription skipped: api not initialized");
        return;
    }

    subscribed_instruments_ = instruments;
    subscription_success_count_.store(0, std::memory_order_relaxed);
    subscription_error_count_.store(0, std::memory_order_relaxed);

    // SimNow fronts do not behave consistently when thousands of contracts are
    // sent in a single SubscribeMarketData call. Batch the requests so switching
    // between 30001/30011 and 40001/40011 always rebuilds subscriptions.
    // SimNow 前置在单次 SubscribeMarketData 下发数千合约时行为不一致，
    // 因此分批请求确保 30001/30011 与 40001/40011 切换时总能重建订阅。
    constexpr size_t kSubscribeBatchSize = 100;
    int failed_batches = 0;
    int sent_batches = 0;
    for (size_t offset = 0; offset < subscribed_instruments_.size(); offset += kSubscribeBatchSize) {
        const size_t end = std::min(offset + kSubscribeBatchSize, subscribed_instruments_.size());
        std::vector<char*> insts;
        insts.reserve(end - offset);
        for (size_t i = offset; i < end; ++i) {
            insts.push_back(const_cast<char*>(subscribed_instruments_[i].c_str()));
        }

        const int ret = api_->SubscribeMarketData(insts.data(), static_cast<int>(insts.size()));
        if (ret != 0) {
            ++failed_batches;
            LOG_ERROR("market data subscription batch failed, ret=" + std::to_string(ret) +
                      " offset=" + std::to_string(offset) +
                      " count=" + std::to_string(insts.size()));
        } else {
            ++sent_batches;
        }

        if (end < subscribed_instruments_.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    if (failed_batches == 0) {
        LOG_INFO("market data subscription requests sent in batches: " +
                 summarize_instruments(subscribed_instruments_) +
                 " batch_size=" + std::to_string(kSubscribeBatchSize) +
                 " batches=" + std::to_string(sent_batches));
    } else {
        LOG_ERROR("market data subscription partially failed: " +
                  summarize_instruments(subscribed_instruments_) +
                  " batch_size=" + std::to_string(kSubscribeBatchSize) +
                  " sent_batches=" + std::to_string(sent_batches) +
                  " failed_batches=" + std::to_string(failed_batches));
    }
}

void CtpMdGateway::subscribe_append(const std::vector<std::string>& instruments) {
    if (instruments.empty()) return;
    if (!api_) {
        LOG_ERROR("market data subscribe_append skipped: api not initialized");
        return;
    }

    // 仅下发增量合约，不重置 subscribed_instruments_/计数器；
    // 仍按 100 一批 + 20ms 间隔以匹配 SimNow 前置流量约束。
    constexpr size_t kSubscribeBatchSize = 100;
    int failed_batches = 0;
    int sent_batches = 0;
    for (size_t offset = 0; offset < instruments.size(); offset += kSubscribeBatchSize) {
        const size_t end = std::min(offset + kSubscribeBatchSize, instruments.size());
        std::vector<char*> insts;
        insts.reserve(end - offset);
        for (size_t i = offset; i < end; ++i) {
            insts.push_back(const_cast<char*>(instruments[i].c_str()));
        }
        const int ret = api_->SubscribeMarketData(insts.data(), static_cast<int>(insts.size()));
        if (ret != 0) {
            ++failed_batches;
            LOG_ERROR("market data subscribe_append batch failed, ret=" + std::to_string(ret) +
                      " offset=" + std::to_string(offset) +
                      " count=" + std::to_string(insts.size()));
        } else {
            ++sent_batches;
        }
        if (end < instruments.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    subscribed_instruments_.insert(subscribed_instruments_.end(),
                                   instruments.begin(), instruments.end());

    LOG_INFO("market data subscribe_append done: appended=" + std::to_string(instruments.size()) +
             " batches=" + std::to_string(sent_batches) +
             " failed_batches=" + std::to_string(failed_batches) +
             " total_subscribed=" + std::to_string(subscribed_instruments_.size()));
}

void CtpMdGateway::unsubscribe(const std::vector<std::string>& instruments) {
    if (!api_ || instruments.empty()) return;
    std::vector<char*> insts;
    insts.reserve(instruments.size());
    for (const auto& s : instruments) {
        insts.push_back(const_cast<char*>(s.c_str()));
    }
    const int ret = api_->UnSubscribeMarketData(insts.data(), static_cast<int>(insts.size()));
    if (ret != 0) {
        LOG_ERROR("market data unsubscribe failed, ret=" + std::to_string(ret));
    } else {
        LOG_INFO("market data unsubscribe sent, count=" + std::to_string(instruments.size()));
    }
}

void CtpMdGateway::stop() {
    if (api_) {
        api_->RegisterSpi(nullptr);
        api_->Release();
        api_ = nullptr;
        logged_in_ = false;
        auto old = status_.exchange(MdGatewayStatus::Disconnected);
        if (old != MdGatewayStatus::Disconnected) notify_status_change(old, MdGatewayStatus::Disconnected);
        LOG_INFO("market data gateway stopped");
    }
}

bool CtpMdGateway::wait_for_login(int timeout_sec) {
    std::unique_lock<std::mutex> lock(login_mtx_);
    const bool ok = login_cv_.wait_for(lock, std::chrono::seconds(timeout_sec),
                                       [this] { return logged_in_.load(); });
    if (!ok) {
        LOG_WARN("market data login wait timeout, front=" + front_ +
                 " timeout_sec=" + std::to_string(timeout_sec));
    }
    return ok;
}

void CtpMdGateway::OnFrontConnected() {
    LOG_INFO("market data front connected, login starts");
    auto old = status_.exchange(MdGatewayStatus::Connected);
    if (old != MdGatewayStatus::Connected) notify_status_change(old, MdGatewayStatus::Connected);

    CThostFtdcReqUserLoginField req{};
    std::strncpy(req.BrokerID, broker_id_.c_str(), sizeof(req.BrokerID) - 1);
    std::strncpy(req.UserID, user_id_.c_str(), sizeof(req.UserID) - 1);
    std::strncpy(req.Password, password_.c_str(), sizeof(req.Password) - 1);

    int ret = api_->ReqUserLogin(&req, ++request_id_);
    if (ret != 0) {
        LOG_ERROR("market data login request failed, ret=" + std::to_string(ret));
    }
}

void CtpMdGateway::OnFrontDisconnected(int nReason) {
    logged_in_ = false;
    auto old = status_.exchange(MdGatewayStatus::Disconnected);
    if (old != MdGatewayStatus::Disconnected) notify_status_change(old, MdGatewayStatus::Disconnected);
    char hex_buf[16];
    std::snprintf(hex_buf, sizeof(hex_buf), "0x%04X", nReason);
    LOG_WARN("market data gateway disconnected, reason=" + std::string(hex_buf));
    if (engine_) {
        engine_->push_runtime_alert("行情网关断开 reason=" + std::string(hex_buf));
    }
}

void CtpMdGateway::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                  CThostFtdcRspInfoField* pRspInfo,
                                  int, bool) {
    if (is_error_rsp(pRspInfo)) {
        LOG_ERROR("market data login failed");
        return;
    }

    if (!pRspUserLogin) {
        LOG_ERROR("market data login failed: pRspUserLogin is null");
        return;
    }

    logged_in_ = true;
    auto old_s = status_.exchange(MdGatewayStatus::LoggedIn);
    if (old_s != MdGatewayStatus::LoggedIn) notify_status_change(old_s, MdGatewayStatus::LoggedIn);
    login_cv_.notify_all();
    LOG_INFO("market data login succeeded, trading_day=" + std::string(pRspUserLogin->TradingDay));

    if (!subscribed_instruments_.empty()) {
        LOG_INFO("market data auto resubscribe after reconnect, count=" +
                 std::to_string(subscribed_instruments_.size()));
        subscribe(subscribed_instruments_);
    }
}

void CtpMdGateway::OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                                      CThostFtdcRspInfoField* pRspInfo,
                                      int, bool) {
    if (is_error_rsp(pRspInfo)) {
        const int errors = subscription_error_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (pSpecificInstrument) {
            LOG_ERROR("market data subscription failed: " +
                      std::string(pSpecificInstrument->InstrumentID) +
                      " errors=" + std::to_string(errors));
        }
        LOG_ERROR("market data subscription failed");
        return;
    }
    const int succeeded = subscription_success_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int target = static_cast<int>(subscribed_instruments_.size());
    if (succeeded == 1 || succeeded == target || succeeded % 1000 == 0) {
        LOG_INFO("market data subscription progress: succeeded=" + std::to_string(succeeded) +
                 "/" + std::to_string(target));
    }
}

void CtpMdGateway::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) {
    if (!pData || !engine_) return;

    // Timestamp the tick immediately upon local receipt for precise gateway->engine latency measurement
    // 本地收到 tick 的第一行打时间戳，用于精确测量 网关→引擎 延迟 (延迟测量)
    const int64_t recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    TickData tick{};
    safe_copy(tick.instrument_id, pData->InstrumentID, sizeof(tick.instrument_id));
    safe_copy(tick.exchange_id, pData->ExchangeID, sizeof(tick.exchange_id));
    tick.last_price = sanitize_price(pData->LastPrice);
    tick.pre_close_price = sanitize_price(pData->PreClosePrice);
    tick.open_price = sanitize_price(pData->OpenPrice);
    tick.highest_price = sanitize_price(pData->HighestPrice);
    tick.lowest_price = sanitize_price(pData->LowestPrice);
    tick.volume = pData->Volume;
    tick.turnover = pData->Turnover;
    tick.open_interest = pData->OpenInterest;
    tick.bid[0] = { sanitize_price(pData->BidPrice1), pData->BidVolume1 };
    tick.ask[0] = { sanitize_price(pData->AskPrice1), pData->AskVolume1 };
    tick.bid[1] = { sanitize_price(pData->BidPrice2), pData->BidVolume2 };
    tick.ask[1] = { sanitize_price(pData->AskPrice2), pData->AskVolume2 };
    tick.bid[2] = { sanitize_price(pData->BidPrice3), pData->BidVolume3 };
    tick.ask[2] = { sanitize_price(pData->AskPrice3), pData->AskVolume3 };
    tick.bid[3] = { sanitize_price(pData->BidPrice4), pData->BidVolume4 };
    tick.ask[3] = { sanitize_price(pData->AskPrice4), pData->AskVolume4 };
    tick.bid[4] = { sanitize_price(pData->BidPrice5), pData->BidVolume5 };
    tick.ask[4] = { sanitize_price(pData->AskPrice5), pData->AskVolume5 };
    tick.upper_limit = sanitize_price(pData->UpperLimitPrice);
    tick.lower_limit = sanitize_price(pData->LowerLimitPrice);
    safe_copy(tick.update_time, pData->UpdateTime, sizeof(tick.update_time));
    tick.update_millisec = pData->UpdateMillisec;
    safe_copy(tick.trading_day, pData->TradingDay, sizeof(tick.trading_day));
    safe_copy(tick.action_day, pData->ActionDay, sizeof(tick.action_day));
    tick.local_recv_ns = recv_ns;

    engine_->on_tick(tick);
}

void CtpMdGateway::OnRspError(CThostFtdcRspInfoField* pRspInfo, int, bool) {
    is_error_rsp(pRspInfo);
}

bool CtpMdGateway::is_error_rsp(CThostFtdcRspInfoField* pRspInfo) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR("market data gateway error: [" + std::to_string(pRspInfo->ErrorID) + "] " +
                  gbk_to_utf8(pRspInfo->ErrorMsg));
        return true;
    }
    return false;
}

} // namespace hft
