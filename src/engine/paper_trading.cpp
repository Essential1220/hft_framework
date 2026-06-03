// ============================================
// paper_trading.cpp - Paper trading / simulation engine implementation (模拟交易引擎实现)
// ============================================

#include "engine/paper_trading.h"
#include "engine/trading_engine.h"
#include "common/logger.h"

#include <cstring>
#include <cstdio>
#include <ctime>

namespace hft {

// Generate current time string for simulated trade timestamps (为模拟成交时间戳生成当前时间字符串)
namespace {
std::string paper_time_text() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}
} // namespace

void PaperTradingEngine::init(TradingEngine* engine) {
    engine_ = engine; // Bind the hosting trading engine (绑定宿主交易引擎)
}

bool PaperTradingEngine::start(const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (active_.load(std::memory_order_relaxed)) return false;
    account_id_ = account_id;
    stats_ = Stats{};
    active_.store(true, std::memory_order_release);
    LOG_INFO("PaperTrading started, account=" + (account_id.empty() ? std::string("default") : account_id));
    return true;
}

void PaperTradingEngine::stop() {
    std::lock_guard<std::mutex> lock(mtx_);
    active_.store(false, std::memory_order_release);
    LOG_INFO("PaperTrading stopped");
}

bool PaperTradingEngine::is_active() const {
    return active_.load(std::memory_order_acquire);
}

std::string PaperTradingEngine::active_account_id() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return account_id_;
}

// Simulate order execution — instant fill at current market price or limit price (模拟订单执行 — 以当前市价或限价即时成交)
SendOrderResult PaperTradingEngine::simulate_order(const OrderRequest& req) {
    SendOrderResult result;

    if (!engine_) {
        result.reject_reason = "EngineNotReady";
        result.reject_message = "paper trading engine not initialized";
        return result;
    }

    if (req.instrument_id[0] == '\0' || req.volume <= 0) {
        result.reject_reason = "InvalidParam";
        result.reject_message = "invalid instrument or volume";
        return result;
    }

    TickData tick = engine_->get_last_tick(req.instrument_id);
    double fill_price = req.price;
    if (tick.last_price > 0) {
        if (req.price_type == OrderRequest::PriceType::Market) {
            fill_price = (req.direction == Direction::Buy) ? tick.ask[0].price : tick.bid[0].price;
            if (fill_price <= 0) fill_price = tick.last_price;
        } else {
            fill_price = req.price;
        }
    }

    const uint32_t seq = paper_ref_seq_.fetch_add(1, std::memory_order_relaxed);
    char ref_buf[16];
    std::snprintf(ref_buf, sizeof(ref_buf), "P%u", seq);
    const std::string order_ref(ref_buf);

    char trade_id_buf[24];
    std::snprintf(trade_id_buf, sizeof(trade_id_buf), "PT%u", seq);

    const std::string time_str = paper_time_text();

    OrderInfo order{};
    std::strncpy(order.instrument_id, req.instrument_id, sizeof(order.instrument_id) - 1);
    std::strncpy(order.exchange_id, req.exchange_id, sizeof(order.exchange_id) - 1);
    std::strncpy(order.account_id, req.account_id, sizeof(order.account_id) - 1);
    std::strncpy(order.order_ref, ref_buf, sizeof(order.order_ref) - 1);
    std::strncpy(order.strategy_id, req.strategy_id, sizeof(order.strategy_id) - 1);
    order.direction = req.direction;
    order.offset = req.offset;
    order.price = fill_price;
    order.total_volume = req.volume;
    order.traded_volume = req.volume;
    order.status = OrderStatus::AllTraded;
    std::strncpy(order.insert_time, time_str.c_str(), sizeof(order.insert_time) - 1);

    TradeInfo trade{};
    std::strncpy(trade.instrument_id, req.instrument_id, sizeof(trade.instrument_id) - 1);
    std::strncpy(trade.exchange_id, req.exchange_id, sizeof(trade.exchange_id) - 1);
    std::strncpy(trade.account_id, req.account_id, sizeof(trade.account_id) - 1);
    std::strncpy(trade.trade_id, trade_id_buf, sizeof(trade.trade_id) - 1);
    std::strncpy(trade.order_ref, ref_buf, sizeof(trade.order_ref) - 1);
    std::strncpy(trade.strategy_id, req.strategy_id, sizeof(trade.strategy_id) - 1);
    trade.direction = req.direction;
    trade.offset = req.offset;
    trade.price = fill_price;
    trade.volume = req.volume;
    std::strncpy(trade.trade_time, time_str.c_str(), sizeof(trade.trade_time) - 1);

    engine_->on_order(order);
    engine_->on_trade(trade);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        stats_.total_orders++;
        stats_.total_fills++;
    }

    result.order_ref = order_ref;
    LOG_INFO("PaperTrading fill: " + std::string(req.instrument_id) + " " +
             (req.direction == Direction::Buy ? "B" : "S") + " " +
             std::to_string(req.volume) + "@" + std::to_string(fill_price) +
             " ref=" + order_ref);
    return result;
}

PaperTradingEngine::Stats PaperTradingEngine::get_stats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

void PaperTradingEngine::reset_stats() {
    std::lock_guard<std::mutex> lock(mtx_);
    stats_ = Stats{};
}

} // namespace hft
