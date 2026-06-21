#include "engine/backtest_engine.h"
#include "engine/tick_reader.h"
#include "strategy/strategy_base.h"
#include "strategy/simple_strategy.h"
#include "common/logger.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace hft {

// ---- BacktestContext implementation ----

void BacktestContext::send_order(const OrderRequest& req) {
    send_order_with_ref(req);
}

std::string BacktestContext::send_order_with_ref(const OrderRequest& req) {
    char ref_buf[16];
    std::snprintf(ref_buf, sizeof(ref_buf), "BT%u", ++order_seq_);

    double fill_price = req.price;
    if (req.price_type == OrderRequest::PriceType::Market) {
        if (req.direction == Direction::Buy && current_tick_.ask[0].price > 0) {
            fill_price = current_tick_.ask[0].price;
        } else if (req.direction == Direction::Sell && current_tick_.bid[0].price > 0) {
            fill_price = current_tick_.bid[0].price;
        } else {
            fill_price = current_tick_.last_price;
        }
    }

    BacktestTrade bt;
    bt.instrument = req.instrument_id;
    bt.direction = req.direction;
    bt.offset = req.offset;
    bt.price = fill_price;
    bt.volume = req.volume;
    bt.time = current_tick_.update_time;
    trades_.push_back(bt);

    // Update net position
    int delta = req.volume;
    if (req.offset != Offset::Open) delta = -delta;
    if (req.direction == Direction::Sell && req.offset == Offset::Open) delta = -delta;
    if (req.direction == Direction::Buy && req.offset != Offset::Open) delta = -delta;
    // Simplified: Buy Open = +vol, Sell Open = -vol, Buy Close = +vol (cover short), Sell Close = -vol
    if (req.offset == Offset::Open) {
        net_positions_[req.instrument_id] += (req.direction == Direction::Buy ? req.volume : -req.volume);
    } else {
        net_positions_[req.instrument_id] += (req.direction == Direction::Buy ? req.volume : -req.volume);
    }

    // Generate order/trade callbacks to strategy
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
    std::strncpy(order.insert_time, current_tick_.update_time, sizeof(order.insert_time) - 1);

    TradeInfo trade{};
    std::strncpy(trade.instrument_id, req.instrument_id, sizeof(trade.instrument_id) - 1);
    std::strncpy(trade.exchange_id, req.exchange_id, sizeof(trade.exchange_id) - 1);
    std::strncpy(trade.account_id, req.account_id, sizeof(trade.account_id) - 1);
    std::strncpy(trade.order_ref, ref_buf, sizeof(trade.order_ref) - 1);
    std::strncpy(trade.strategy_id, req.strategy_id, sizeof(trade.strategy_id) - 1);
    char trade_id[24];
    std::snprintf(trade_id, sizeof(trade_id), "BT%u", order_seq_);
    std::strncpy(trade.trade_id, trade_id, sizeof(trade.trade_id) - 1);
    trade.direction = req.direction;
    trade.offset = req.offset;
    trade.price = fill_price;
    trade.volume = req.volume;
    std::strncpy(trade.trade_time, current_tick_.update_time, sizeof(trade.trade_time) - 1);

    if (strategy_) {
        strategy_->on_order(order);
        strategy_->on_trade(trade);
    }

    return ref_buf;
}

void BacktestContext::cancel_order(const std::string&) {}
bool BacktestContext::cancel_order(const std::string&, const std::string&) { return true; }
uint32_t BacktestContext::add_conditional_order(const ConditionalOrder&) { return 0; }
void BacktestContext::cancel_conditional_order(uint32_t) {}
uint32_t BacktestContext::allocate_cond_group_id() { return ++group_seq_; }

PositionInfo BacktestContext::get_position(const char* instrument, Direction dir,
                                           const std::string&) const {
    PositionInfo pos{};
    std::strncpy(pos.instrument_id, instrument, sizeof(pos.instrument_id) - 1);
    pos.direction = dir;
    auto it = net_positions_.find(instrument);
    if (it != net_positions_.end()) {
        if (dir == Direction::Buy && it->second > 0) {
            pos.total = it->second;
        } else if (dir == Direction::Sell && it->second < 0) {
            pos.total = -it->second;
        }
    }
    return pos;
}

int BacktestContext::get_net_position(const char* instrument, const std::string&) const {
    auto it = net_positions_.find(instrument);
    return (it != net_positions_.end()) ? it->second : 0;
}

WindowedOrderBook BacktestContext::get_order_book(const char*) const {
    return WindowedOrderBook{};
}

AccountInfo BacktestContext::get_account_info(const std::string&) const {
    AccountInfo info{};
    info.balance = 1000000.0;
    info.available = 1000000.0;
    return info;
}

void BacktestContext::strategy_log(const std::string&, int, const std::string& message) {
    LOG_INFO("backtest strategy: " + message);
}

void BacktestContext::save_strategy_state(const std::string& strategy_id,
                                          const std::map<std::string, std::string>& state) {
    saved_states_[strategy_id] = state;
}

std::map<std::string, std::string> BacktestContext::load_strategy_state(const std::string& strategy_id) {
    auto it = saved_states_.find(strategy_id);
    return (it != saved_states_.end()) ? it->second : std::map<std::string, std::string>{};
}

int BacktestContext::register_timer(const std::string&, int) { return -1; }
void BacktestContext::unregister_timer(int) {}
std::vector<KlineBar> BacktestContext::query_klines(const std::string&, const std::string&,
                                                     size_t) const {
    return {};
}

// ---- BacktestEngine implementation ----

bool BacktestEngine::run(const BacktestConfig& config) {
    report_ = BacktestReport{};

    if (config.tick_files.empty()) {
        LOG_ERROR("backtest: no tick files specified");
        return false;
    }
    if (config.instrument.empty()) {
        LOG_ERROR("backtest: no instrument specified");
        return false;
    }

    // 1. Load tick data
    std::vector<std::vector<TickData>> all_ticks;
    for (const auto& path : config.tick_files) {
        std::vector<TickData> ticks;
        if (!read_htick_file(path, ticks)) {
            LOG_ERROR("backtest: failed to read " + path);
            return false;
        }
        LOG_INFO("backtest: loaded " + std::to_string(ticks.size()) + " ticks from " + path);
        all_ticks.push_back(std::move(ticks));
    }

    std::vector<TickData> merged;
    merge_ticks_by_time(all_ticks, merged);
    all_ticks.clear();

    if (merged.empty()) {
        LOG_ERROR("backtest: no ticks loaded");
        return false;
    }

    // 2. Create strategy
    std::shared_ptr<StrategyBase> strategy;
    if (config.strategy_type == "simple" || config.strategy_type.empty()) {
        strategy = std::make_shared<SimpleStrategy>(
            config.instrument.c_str(), config.order_size, 3, 5);
    } else {
        LOG_ERROR("backtest: unsupported strategy type '" + config.strategy_type +
                  "' (only 'simple' supported in backtest mode)");
        return false;
    }

    // 3. Set up context
    BacktestContext ctx;
    ctx.set_strategy(strategy.get());
    strategy->set_engine(&ctx);
    strategy->configure_context("bt_strategy", "BACKTEST", {config.instrument});
    strategy->configure_metadata(config.strategy_type, config.script_path, {});

    // 4. Run
    LOG_INFO("backtest: replaying " + std::to_string(merged.size()) + " ticks on " + config.instrument);
    auto t0 = std::chrono::steady_clock::now();

    strategy->on_init();

    for (const auto& tick : merged) {
        ctx.set_current_tick(tick);
        if (strategy->handles_instrument(tick.instrument_id)) {
            strategy->on_tick(tick);
        }
    }

    strategy->on_stop();

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 5. Generate report
    report_ = compute_backtest_report(ctx.trades(), static_cast<int>(merged.size()));

    LOG_INFO("backtest: completed in " + std::to_string(elapsed_ms) + " ms, " +
             std::to_string(ctx.trades().size()) + " trades");

    printf("%s", report_.to_string().c_str());

    return true;
}

} // namespace hft
