// ============================================
// hft_api.cpp - C API implementation (C 语言接口实现)
// Bridges extern "C" functions to TradingEngine via opaque handle.
// ============================================

#include "api/hft_api.h"
#include "api/json_builder.h"
#include "engine/trading_engine.h"
#include "common/logger.h"

#include <cstring>
#include <new>

struct HftEngine {
    hft::TradingEngine engine;
};

// ---- helpers ----

static hft::TradingEngine* to_engine(HftEngineHandle h) {
    return h ? &h->engine : nullptr;
}

static char* empty_json_object() {
    char* s = new(std::nothrow) char[3];
    if (s) { s[0] = '{'; s[1] = '}'; s[2] = '\0'; }
    return s;
}

static char* empty_json_array() {
    char* s = new(std::nothrow) char[3];
    if (s) { s[0] = '['; s[1] = ']'; s[2] = '\0'; }
    return s;
}

static const char* safe_str(const char* s) { return s ? s : ""; }

static const char* direction_str(hft::Direction d) {
    return d == hft::Direction::Buy ? "buy" : "sell";
}

static const char* offset_str(hft::Offset o) {
    switch (o) {
        case hft::Offset::Open:           return "open";
        case hft::Offset::Close:          return "close";
        case hft::Offset::CloseToday:     return "close_today";
        case hft::Offset::CloseYesterday: return "close_yesterday";
        default:                          return "unknown";
    }
}

static const char* order_status_str(hft::OrderStatus s) {
    switch (s) {
        case hft::OrderStatus::Pending:      return "pending";
        case hft::OrderStatus::PartTraded:   return "part_traded";
        case hft::OrderStatus::AllTraded:    return "all_traded";
        case hft::OrderStatus::Cancelled:    return "cancelled";
        case hft::OrderStatus::Error:        return "error";
        case hft::OrderStatus::Created:      return "created";
        case hft::OrderStatus::RiskRejected: return "risk_rejected";
        case hft::OrderStatus::CancelPending:return "cancel_pending";
        case hft::OrderStatus::Submitted:    return "submitted";
        default:                             return "unknown";
    }
}

static const char* risk_mode_str(hft::RiskMode m) {
    switch (m) {
        case hft::RiskMode::Normal:      return "normal";
        case hft::RiskMode::Warning:     return "warning";
        case hft::RiskMode::NoOpen:      return "no_open";
        case hft::RiskMode::ReduceOnly:  return "reduce_only";
        case hft::RiskMode::Liquidating: return "liquidating";
        case hft::RiskMode::Halted:      return "halted";
        default:                         return "unknown";
    }
}

// ============================================
// Lifecycle
// ============================================

HftEngineHandle hft_engine_create(void) {
    return new(std::nothrow) HftEngine();
}

int hft_engine_init(HftEngineHandle h, const char* config_path) {
    auto* e = to_engine(h);
    if (!e) return -1;
    return e->init(safe_str(config_path)) ? 0 : -1;
}

int hft_engine_start(HftEngineHandle h) {
    auto* e = to_engine(h);
    if (!e) return -1;
    return e->start() ? 0 : -1;
}

void hft_engine_stop(HftEngineHandle h) {
    auto* e = to_engine(h);
    if (e) e->stop();
}

void hft_engine_destroy(HftEngineHandle h) {
    delete h;
}

// ============================================
// Trading
// ============================================

int hft_send_order(HftEngineHandle h, const char* instrument,
                   int direction, int offset, double price,
                   int volume, char* out_ref, int ref_len) {
    auto* e = to_engine(h);
    if (!e) return -1;

    hft::OrderRequest req{};
    std::strncpy(req.instrument_id, safe_str(instrument), sizeof(req.instrument_id) - 1);
    req.direction = static_cast<hft::Direction>(direction);
    req.offset    = static_cast<hft::Offset>(offset);
    req.price     = price;
    req.volume    = volume;

    auto result = e->send_order_with_result(req);
    if (!result.success()) return -1;

    if (out_ref && ref_len > 0) {
        std::strncpy(out_ref, result.order_ref.c_str(), static_cast<size_t>(ref_len) - 1);
        out_ref[ref_len - 1] = '\0';
    }
    return 0;
}

int hft_cancel_order(HftEngineHandle h, const char* order_ref) {
    auto* e = to_engine(h);
    if (!e || !order_ref) return -1;
    e->cancel_order(std::string(order_ref));
    return 0;
}

int hft_cancel_all(HftEngineHandle h, const char* account_id) {
    auto* e = to_engine(h);
    if (!e) return -1;
    return static_cast<int>(e->cancel_all_orders(safe_str(account_id)));
}

// ============================================
// Query — Account
// ============================================

char* hft_get_account(HftEngineHandle h, const char* account_id) {
    auto* e = to_engine(h);
    if (!e) return empty_json_object();

    auto a = e->get_account(safe_str(account_id));
    hft::JsonBuilder jb;
    jb.begin_object()
      .kv("account_id",       a.account_id)
      .kv("balance",          a.balance)
      .kv("available",        a.available)
      .kv("margin",           a.margin)
      .kv("commission",       a.commission)
      .kv("close_profit",     a.close_profit)
      .kv("position_profit",  a.position_profit)
      .kv("frozen_margin",    a.frozen_margin)
      .kv("frozen_commission",a.frozen_commission)
      .end_object();
    return jb.to_cstr();
}

// ============================================
// Query — Positions
// ============================================

char* hft_get_positions(HftEngineHandle h, const char* account_id) {
    auto* e = to_engine(h);
    if (!e) return empty_json_array();

    auto positions = e->get_all_positions(safe_str(account_id));
    hft::JsonBuilder jb;
    jb.begin_array();
    for (const auto& p : positions) {
        jb.begin_object()
          .kv("instrument_id",  p.instrument_id)
          .kv("account_id",     p.account_id)
          .kv("direction",      direction_str(p.direction))
          .kv("total",          p.total)
          .kv("today",          p.today)
          .kv("yesterday",      p.yesterday)
          .kv("avg_price",      p.avg_price)
          .kv("position_profit",p.position_profit)
          .kv("use_margin",     p.use_margin)
          .end_object();
    }
    jb.end_array();
    return jb.to_cstr();
}

// ============================================
// Query — Active Orders
// ============================================

char* hft_get_active_orders(HftEngineHandle h, const char* account_id) {
    auto* e = to_engine(h);
    if (!e) return empty_json_array();

    auto orders = e->get_active_orders(safe_str(account_id));
    hft::JsonBuilder jb;
    jb.begin_array();
    for (const auto& o : orders) {
        jb.begin_object()
          .kv("instrument_id",  o.instrument_id)
          .kv("exchange_id",    o.exchange_id)
          .kv("account_id",     o.account_id)
          .kv("order_ref",      o.order_ref)
          .kv("strategy_id",    o.strategy_id)
          .kv("order_sys_id",   o.order_sys_id)
          .kv("direction",      direction_str(o.direction))
          .kv("offset",         offset_str(o.offset))
          .kv("price",          o.price)
          .kv("total_volume",   o.total_volume)
          .kv("traded_volume",  o.traded_volume)
          .kv("status",         order_status_str(o.status))
          .kv("status_msg",     o.status_msg)
          .kv("insert_time",    o.insert_time)
          .end_object();
    }
    jb.end_array();
    return jb.to_cstr();
}

// ============================================
// Query — Last Tick
// ============================================

char* hft_get_last_tick(HftEngineHandle h, const char* instrument) {
    auto* e = to_engine(h);
    if (!e || !instrument) return empty_json_object();

    auto t = e->get_last_tick(instrument);
    hft::JsonBuilder jb;
    jb.begin_object()
      .kv("instrument_id",   t.instrument_id)
      .kv("exchange_id",     t.exchange_id)
      .kv("last_price",      t.last_price)
      .kv("pre_close_price", t.pre_close_price)
      .kv("open_price",      t.open_price)
      .kv("highest_price",   t.highest_price)
      .kv("lowest_price",    t.lowest_price)
      .kv("volume",          t.volume)
      .kv("turnover",        t.turnover)
      .kv("open_interest",   t.open_interest)
      .kv("upper_limit",     t.upper_limit)
      .kv("lower_limit",     t.lower_limit)
      .kv("update_time",     t.update_time)
      .kv("update_millisec", t.update_millisec)
      .kv("trading_day",     t.trading_day);

    jb.key("bid"); jb.begin_array();
    for (int i = 0; i < 5; ++i) {
        jb.begin_object()
          .kv("price",  t.bid[i].price)
          .kv("volume", t.bid[i].volume)
          .end_object();
    }
    jb.end_array();

    jb.key("ask"); jb.begin_array();
    for (int i = 0; i < 5; ++i) {
        jb.begin_object()
          .kv("price",  t.ask[i].price)
          .kv("volume", t.ask[i].volume)
          .end_object();
    }
    jb.end_array();

    jb.end_object();
    return jb.to_cstr();
}

// ============================================
// Query — Latency
// ============================================

char* hft_get_latency(HftEngineHandle h) {
    auto* e = to_engine(h);
    if (!e) return empty_json_object();

    auto lat = e->get_latency_snapshot();
    hft::JsonBuilder jb;
    jb.begin_object()
      .kv("tick_to_signal_us",  lat.tick_to_signal_us)
      .kv("signal_to_order_us", lat.signal_to_order_us)
      .kv("order_to_trade_us",  lat.order_to_trade_us)
      .kv("tick_process_us",    lat.tick_process_us)
      .kv("order_process_us",   lat.order_process_us)
      .kv("trade_process_us",   lat.trade_process_us)
      .end_object();
    return jb.to_cstr();
}

// ============================================
// Query — PnL Curve
// ============================================

char* hft_get_pnl_curve(HftEngineHandle h, int limit) {
    auto* e = to_engine(h);
    if (!e) return empty_json_array();

    auto curve = e->get_pnl_curve(limit > 0 ? static_cast<size_t>(limit) : 240);
    hft::JsonBuilder jb;
    jb.begin_array();
    for (const auto& pt : curve) {
        jb.begin_object()
          .kv("timestamp_ms",    pt.timestamp_ms)
          .kv("time",            pt.time)
          .kv("balance",         pt.balance)
          .kv("available",       pt.available)
          .kv("margin",          pt.margin)
          .kv("position_profit", pt.position_profit)
          .kv("total_pnl",       pt.total_pnl)
          .end_object();
    }
    jb.end_array();
    return jb.to_cstr();
}

// ============================================
// Query — Strategy Snapshots
// ============================================

char* hft_get_strategy_snapshots(HftEngineHandle h) {
    auto* e = to_engine(h);
    if (!e) return empty_json_array();

    auto snaps = e->get_strategy_snapshots();
    hft::JsonBuilder jb;
    jb.begin_array();
    for (const auto& s : snaps) {
        jb.begin_object()
          .kv("strategy_id",     s.strategy_id)
          .kv("strategy_type",   s.strategy_type)
          .kv("version",         s.version)
          .kv("status",          s.status)
          .kv("trade_count",     s.trade_count)
          .kv("signal_count",    s.signal_count)
          .kv("position_volume", s.position_volume)
          .kv("avg_price",       s.avg_price)
          .kv("realized_pnl",    s.realized_pnl)
          .kv("floating_pnl",    s.floating_pnl)
          .kv("total_pnl",       s.total_pnl)
          .kv("win_rate",        s.win_rate)
          .kv("profit_factor",   s.profit_factor)
          .kv("last_signal",     s.last_signal)
          .kv("last_signal_time",s.last_signal_time)
          .end_object();
    }
    jb.end_array();
    return jb.to_cstr();
}

// ============================================
// Query — Account Snapshots
// ============================================

char* hft_get_account_snapshots(HftEngineHandle h) {
    auto* e = to_engine(h);
    if (!e) return empty_json_array();

    auto snaps = e->get_account_snapshots();
    hft::JsonBuilder jb;
    jb.begin_array();
    for (const auto& a : snaps) {
        jb.begin_object()
          .kv("account_id",              a.account_id)
          .kv("balance",                 a.account.balance)
          .kv("available",               a.account.available)
          .kv("margin",                  a.account.margin)
          .kv("commission",              a.account.commission)
          .kv("close_profit",            a.account.close_profit)
          .kv("position_profit",         a.account.position_profit)
          .kv("position_count",          a.position_count)
          .kv("active_order_count",      a.active_order_count)
          .kv("trade_gateway_logged_in", a.trade_gateway_logged_in)
          .kv("snapshot_ready",          a.snapshot_ready)
          .kv("trade_state",             a.trade_state)
          .end_object();
    }
    jb.end_array();
    return jb.to_cstr();
}

// ============================================
// Query — Risk Snapshot
// ============================================

char* hft_get_risk_snapshot(HftEngineHandle h, const char* account_id) {
    auto* e = to_engine(h);
    if (!e) return empty_json_object();

    auto r = e->get_risk_snapshot(safe_str(account_id));
    hft::JsonBuilder jb;
    jb.begin_object()
      .kv("risk_level",              r.risk_level)
      .kv("rms_mode",                risk_mode_str(r.rms_mode))
      .kv("max_order_size",          r.max_order_size)
      .kv("max_net_position",        r.max_net_position)
      .kv("order_freq_limit",        r.order_freq_limit)
      .kv("current_order_freq",      r.current_order_freq)
      .kv("cancel_rate_limit",       r.cancel_rate_limit)
      .kv("current_cancel_rate",     r.current_cancel_rate)
      .kv("max_daily_loss",          r.max_daily_loss)
      .kv("current_daily_loss",      r.current_daily_loss)
      .kv("margin_usage_ratio",      r.margin_usage_ratio)
      .kv("last_reject_reason",      r.last_reject_reason)
      .kv("total_rejects_today",     r.total_rejects_today)
      .end_object();
    return jb.to_cstr();
}

// ============================================
// Query — Recent Alerts
// ============================================

char* hft_get_recent_alerts(HftEngineHandle h, int limit) {
    auto* e = to_engine(h);
    if (!e) return empty_json_array();

    auto alerts = e->get_recent_alerts(limit > 0 ? static_cast<size_t>(limit) : 50);
    hft::JsonBuilder jb;
    jb.begin_array();
    for (const auto& msg : alerts) {
        jb.value(msg);
    }
    jb.end_array();
    return jb.to_cstr();
}

// ============================================
// Query — Recent Orders
// ============================================

char* hft_get_recent_orders(HftEngineHandle h, const char* account_id, int limit) {
    auto* e = to_engine(h);
    if (!e) return empty_json_array();

    auto orders = e->get_recent_orders(safe_str(account_id),
                                       limit > 0 ? static_cast<size_t>(limit) : 200);
    hft::JsonBuilder jb;
    jb.begin_array();
    for (const auto& o : orders) {
        jb.begin_object()
          .kv("instrument_id",  o.instrument_id)
          .kv("exchange_id",    o.exchange_id)
          .kv("account_id",     o.account_id)
          .kv("order_ref",      o.order_ref)
          .kv("strategy_id",    o.strategy_id)
          .kv("order_sys_id",   o.order_sys_id)
          .kv("direction",      direction_str(o.direction))
          .kv("offset",         offset_str(o.offset))
          .kv("price",          o.price)
          .kv("total_volume",   o.total_volume)
          .kv("traded_volume",  o.traded_volume)
          .kv("status",         order_status_str(o.status))
          .kv("status_msg",     o.status_msg)
          .kv("insert_time",    o.insert_time)
          .end_object();
    }
    jb.end_array();
    return jb.to_cstr();
}

// ============================================
// Query — Recent Trades
// ============================================

char* hft_get_recent_trades(HftEngineHandle h, const char* account_id, int limit) {
    auto* e = to_engine(h);
    if (!e) return empty_json_array();

    auto trades = e->get_recent_trades(safe_str(account_id),
                                       limit > 0 ? static_cast<size_t>(limit) : 200);
    hft::JsonBuilder jb;
    jb.begin_array();
    for (const auto& t : trades) {
        jb.begin_object()
          .kv("instrument_id", t.instrument_id)
          .kv("exchange_id",   t.exchange_id)
          .kv("account_id",    t.account_id)
          .kv("trade_id",      t.trade_id)
          .kv("order_ref",     t.order_ref)
          .kv("strategy_id",   t.strategy_id)
          .kv("direction",     direction_str(t.direction))
          .kv("offset",        offset_str(t.offset))
          .kv("price",         t.price)
          .kv("volume",        t.volume)
          .kv("trade_time",    t.trade_time)
          .end_object();
    }
    jb.end_array();
    return jb.to_cstr();
}

// ============================================
// State
// ============================================

int hft_is_running(HftEngineHandle h) {
    auto* e = to_engine(h);
    return (e && e->is_running()) ? 1 : 0;
}

int hft_is_trading_ready(HftEngineHandle h) {
    auto* e = to_engine(h);
    return (e && e->is_trading_ready()) ? 1 : 0;
}

int hft_get_risk_mode(HftEngineHandle h) {
    auto* e = to_engine(h);
    if (!e) return 0;
    return static_cast<int>(e->get_risk_mode());
}

void hft_set_risk_mode(HftEngineHandle h, int mode, const char* reason) {
    auto* e = to_engine(h);
    if (!e) return;
    e->set_risk_mode(static_cast<hft::RiskMode>(mode), safe_str(reason));
}

// ============================================
// Memory
// ============================================

void hft_free_string(char* s) {
    delete[] s;
}
