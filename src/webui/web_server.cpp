// ============================================
// web_server.cpp - HTTP server implementation
// Prometheus /metrics + REST API + embedded WebUI dashboard
// ============================================

#ifdef ENABLE_METRICS

#include "webui/web_server.h"
#include "webui/embedded_html.h"
#include "api/json_builder.h"
#include "engine/trading_engine.h"
#include "common/logger.h"

#include <httplib.h>
#include <sstream>

namespace hft {

// ---- enum-to-string helpers ----

static const char* direction_str(Direction d) {
    return d == Direction::Buy ? "buy" : "sell";
}

static const char* offset_str(Offset o) {
    switch (o) {
        case Offset::Open:           return "open";
        case Offset::Close:          return "close";
        case Offset::CloseToday:     return "close_today";
        case Offset::CloseYesterday: return "close_yesterday";
        default:                     return "unknown";
    }
}

static const char* order_status_str(OrderStatus s) {
    switch (s) {
        case OrderStatus::Pending:      return "pending";
        case OrderStatus::PartTraded:   return "part_traded";
        case OrderStatus::AllTraded:    return "all_traded";
        case OrderStatus::Cancelled:    return "cancelled";
        case OrderStatus::Error:        return "error";
        case OrderStatus::Created:      return "created";
        case OrderStatus::RiskRejected: return "risk_rejected";
        case OrderStatus::CancelPending:return "cancel_pending";
        case OrderStatus::Submitted:    return "submitted";
        default:                        return "unknown";
    }
}

static const char* risk_mode_str(RiskMode m) {
    switch (m) {
        case RiskMode::Normal:      return "normal";
        case RiskMode::Warning:     return "warning";
        case RiskMode::NoOpen:      return "no_open";
        case RiskMode::ReduceOnly:  return "reduce_only";
        case RiskMode::Liquidating: return "liquidating";
        case RiskMode::Halted:      return "halted";
        default:                    return "unknown";
    }
}

static std::string escape_prom_label(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// ============================================
// Lifecycle
// ============================================

WebServer::WebServer() = default;

WebServer::~WebServer() {
    stop();
}

void WebServer::start(int port, TradingEngine* engine, bool enable_control) {
    if (server_) return;
    engine_ = engine;
    port_ = port;
    enable_control_ = enable_control;
    start_time_ = std::chrono::steady_clock::now();
    server_ = std::make_unique<httplib::Server>();

    register_metrics_routes();
    register_api_routes();
    register_static_routes();

    server_thread_ = std::thread([this]() {
        LOG_INFO("WebServer listening on port " + std::to_string(port_));
        server_->listen("0.0.0.0", port_);
    });
}

void WebServer::stop() {
    if (server_) {
        server_->stop();
        if (server_thread_.joinable())
            server_thread_.join();
        server_.reset();
        LOG_INFO("WebServer stopped");
    }
}

// ============================================
// Prometheus /metrics (text exposition format)
// ============================================

void WebServer::register_metrics_routes() {
    server_->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(render_metrics(), "text/plain; version=0.0.4; charset=utf-8");
    });
    server_->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });
}

std::string WebServer::render_metrics() const {
    if (!engine_) return "";
    std::ostringstream ss;

    auto lat = engine_->get_latency_snapshot();
    ss << "# HELP hft_tick_to_signal_us Last tick-to-signal latency in microseconds\n"
       << "# TYPE hft_tick_to_signal_us gauge\n"
       << "hft_tick_to_signal_us " << lat.tick_to_signal_us << "\n\n"
       << "# HELP hft_signal_to_order_us Last signal-to-order latency in microseconds\n"
       << "# TYPE hft_signal_to_order_us gauge\n"
       << "hft_signal_to_order_us " << lat.signal_to_order_us << "\n\n"
       << "# HELP hft_order_to_trade_us Last order-to-trade latency in microseconds\n"
       << "# TYPE hft_order_to_trade_us gauge\n"
       << "hft_order_to_trade_us " << lat.order_to_trade_us << "\n\n"
       << "# HELP hft_tick_process_us Last tick processing latency in microseconds\n"
       << "# TYPE hft_tick_process_us gauge\n"
       << "hft_tick_process_us " << lat.tick_process_us << "\n\n";

    ss << "# HELP hft_md_queue_drops_total Total MD queue drops\n"
       << "# TYPE hft_md_queue_drops_total counter\n"
       << "hft_md_queue_drops_total " << engine_->md_queue_drop_count() << "\n\n"
       << "# HELP hft_md_queue_overflow Queue overflow flag (1=overflow detected)\n"
       << "# TYPE hft_md_queue_overflow gauge\n"
       << "hft_md_queue_overflow " << (engine_->has_md_queue_overflow() ? 1 : 0) << "\n\n";

    ss << "# HELP hft_risk_mode Current RMS risk mode (0=Normal..5=Halted)\n"
       << "# TYPE hft_risk_mode gauge\n"
       << "hft_risk_mode " << static_cast<int>(engine_->get_risk_mode()) << "\n\n";

    auto accounts = engine_->get_account_snapshots();
    if (!accounts.empty()) {
        ss << "# HELP hft_account_balance Account balance\n"
           << "# TYPE hft_account_balance gauge\n";
        for (const auto& a : accounts) {
            auto aid = escape_prom_label(a.account_id);
            ss << "hft_account_balance{account=\"" << aid << "\"} " << a.account.balance << "\n";
        }
        ss << "\n# HELP hft_account_available Account available funds\n"
           << "# TYPE hft_account_available gauge\n";
        for (const auto& a : accounts) {
            auto aid = escape_prom_label(a.account_id);
            ss << "hft_account_available{account=\"" << aid << "\"} " << a.account.available << "\n";
        }
        ss << "\n# HELP hft_account_margin Account margin used\n"
           << "# TYPE hft_account_margin gauge\n";
        for (const auto& a : accounts) {
            auto aid = escape_prom_label(a.account_id);
            ss << "hft_account_margin{account=\"" << aid << "\"} " << a.account.margin << "\n";
        }
        ss << "\n# HELP hft_account_position_profit Account position (floating) profit\n"
           << "# TYPE hft_account_position_profit gauge\n";
        for (const auto& a : accounts) {
            auto aid = escape_prom_label(a.account_id);
            ss << "hft_account_position_profit{account=\"" << aid << "\"} " << a.account.position_profit << "\n";
        }
        ss << "\n";
    }

    auto strategies = engine_->get_strategy_performance();
    if (!strategies.empty()) {
        ss << "# HELP hft_strategy_trades_total Per-strategy trade count\n"
           << "# TYPE hft_strategy_trades_total counter\n";
        for (const auto& s : strategies) {
            auto sid = escape_prom_label(s.strategy_id);
            ss << "hft_strategy_trades_total{strategy=\"" << sid << "\"} " << s.trade_count << "\n";
        }
        ss << "\n# HELP hft_strategy_pnl Per-strategy total P&L\n"
           << "# TYPE hft_strategy_pnl gauge\n";
        for (const auto& s : strategies) {
            auto sid = escape_prom_label(s.strategy_id);
            ss << "hft_strategy_pnl{strategy=\"" << sid << "\"} " << s.total_pnl << "\n";
        }
        ss << "\n# HELP hft_strategy_win_rate Per-strategy win rate\n"
           << "# TYPE hft_strategy_win_rate gauge\n";
        for (const auto& s : strategies) {
            auto sid = escape_prom_label(s.strategy_id);
            ss << "hft_strategy_win_rate{strategy=\"" << sid << "\"} " << s.win_rate << "\n";
        }
        ss << "\n";
    }

    auto pnl = engine_->get_pnl_curve(1);
    if (!pnl.empty()) {
        ss << "# HELP hft_daily_pnl Current daily P&L\n"
           << "# TYPE hft_daily_pnl gauge\n"
           << "hft_daily_pnl " << pnl.back().total_pnl << "\n\n";
    }

    ss << "# HELP hft_engine_running Whether engine is running (1=yes)\n"
       << "# TYPE hft_engine_running gauge\n"
       << "hft_engine_running " << (engine_->is_running() ? 1 : 0) << "\n\n"
       << "# HELP hft_trading_ready Whether trading is ready (1=yes)\n"
       << "# TYPE hft_trading_ready gauge\n"
       << "hft_trading_ready " << (engine_->is_trading_ready() ? 1 : 0) << "\n\n";

    return ss.str();
}

// ============================================
// REST API
// ============================================

void WebServer::register_api_routes() {
    // CORS header for all API responses
    auto set_json = [](httplib::Response& res, const std::string& body) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(body, "application/json");
    };

    // GET /api/status
    server_->Get("/api/status", [this, set_json](const httplib::Request&, httplib::Response& res) {
        if (!engine_) { set_json(res, "{}"); return; }
        auto elapsed = std::chrono::steady_clock::now() - start_time_;
        int uptime_s = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());

        JsonBuilder jb;
        jb.begin_object()
          .kv("running",            engine_->is_running())
          .kv("trading_ready",      engine_->is_trading_ready())
          .kv("risk_mode",          risk_mode_str(engine_->get_risk_mode()))
          .kv("uptime_seconds",     uptime_s)
          .kv("md_queue_drops",     static_cast<int64_t>(engine_->md_queue_drop_count()))
          .kv("md_queue_overflow",  engine_->has_md_queue_overflow())
          .end_object();
        set_json(res, jb.build());
    });

    // GET /api/accounts
    server_->Get("/api/accounts", [this, set_json](const httplib::Request&, httplib::Response& res) {
        if (!engine_) { set_json(res, "[]"); return; }
        auto snaps = engine_->get_account_snapshots();
        JsonBuilder jb;
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
        set_json(res, jb.build());
    });

    // GET /api/strategies
    server_->Get("/api/strategies", [this, set_json](const httplib::Request&, httplib::Response& res) {
        if (!engine_) { set_json(res, "[]"); return; }
        auto snaps = engine_->get_strategy_snapshots();
        JsonBuilder jb;
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
        set_json(res, jb.build());
    });

    // GET /api/latency
    server_->Get("/api/latency", [this, set_json](const httplib::Request&, httplib::Response& res) {
        if (!engine_) { set_json(res, "{}"); return; }
        auto lat = engine_->get_latency_snapshot();
        JsonBuilder jb;
        jb.begin_object()
          .kv("tick_to_signal_us",  lat.tick_to_signal_us)
          .kv("signal_to_order_us", lat.signal_to_order_us)
          .kv("order_to_trade_us",  lat.order_to_trade_us)
          .kv("tick_process_us",    lat.tick_process_us)
          .kv("order_process_us",   lat.order_process_us)
          .kv("trade_process_us",   lat.trade_process_us)
          .end_object();
        set_json(res, jb.build());
    });

    // GET /api/pnl
    server_->Get("/api/pnl", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!engine_) { set_json(res, "[]"); return; }
        int limit = 240;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        auto curve = engine_->get_pnl_curve(limit > 0 ? static_cast<size_t>(limit) : 240);
        JsonBuilder jb;
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
        set_json(res, jb.build());
    });

    // GET /api/ticks
    server_->Get("/api/ticks", [this, set_json](const httplib::Request&, httplib::Response& res) {
        if (!engine_) { set_json(res, "[]"); return; }
        auto ticks = engine_->get_subscribed_ticks();
        JsonBuilder jb;
        jb.begin_array();
        for (const auto& t : ticks) {
            jb.begin_object()
              .kv("instrument_id",   t.instrument_id)
              .kv("exchange_id",     t.exchange_id)
              .kv("last_price",      t.last_price)
              .kv("volume",          t.volume)
              .kv("turnover",        t.turnover)
              .kv("open_interest",   t.open_interest)
              .kv("bid_price1",      t.bid[0].price)
              .kv("bid_volume1",     t.bid[0].volume)
              .kv("ask_price1",      t.ask[0].price)
              .kv("ask_volume1",     t.ask[0].volume)
              .kv("upper_limit",     t.upper_limit)
              .kv("lower_limit",     t.lower_limit)
              .kv("update_time",     t.update_time)
              .kv("update_millisec", t.update_millisec)
              .end_object();
        }
        jb.end_array();
        set_json(res, jb.build());
    });

    // GET /api/orders
    server_->Get("/api/orders", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!engine_) { set_json(res, "[]"); return; }
        int limit = 50;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        auto orders = engine_->get_recent_orders("", limit > 0 ? static_cast<size_t>(limit) : 50);
        JsonBuilder jb;
        jb.begin_array();
        for (const auto& o : orders) {
            jb.begin_object()
              .kv("instrument_id",  o.instrument_id)
              .kv("exchange_id",    o.exchange_id)
              .kv("account_id",     o.account_id)
              .kv("order_ref",      o.order_ref)
              .kv("strategy_id",    o.strategy_id)
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
        set_json(res, jb.build());
    });

    // GET /api/trades
    server_->Get("/api/trades", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!engine_) { set_json(res, "[]"); return; }
        int limit = 50;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        auto trades = engine_->get_recent_trades("", limit > 0 ? static_cast<size_t>(limit) : 50);
        JsonBuilder jb;
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
        set_json(res, jb.build());
    });

    // GET /api/alerts
    server_->Get("/api/alerts", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!engine_) { set_json(res, "[]"); return; }
        int limit = 50;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        auto alerts = engine_->get_recent_alerts(limit > 0 ? static_cast<size_t>(limit) : 50);
        JsonBuilder jb;
        jb.begin_array();
        for (const auto& msg : alerts) {
            jb.value(msg);
        }
        jb.end_array();
        set_json(res, jb.build());
    });

    // GET /api/risk
    server_->Get("/api/risk", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!engine_) { set_json(res, "{}"); return; }
        std::string account_id;
        if (req.has_param("account_id"))
            account_id = req.get_param_value("account_id");
        auto r = engine_->get_risk_snapshot(account_id);
        JsonBuilder jb;
        jb.begin_object()
          .kv("risk_level",          r.risk_level)
          .kv("rms_mode",            risk_mode_str(r.rms_mode))
          .kv("max_order_size",      r.max_order_size)
          .kv("max_net_position",    r.max_net_position)
          .kv("order_freq_limit",    r.order_freq_limit)
          .kv("current_order_freq",  r.current_order_freq)
          .kv("cancel_rate_limit",   r.cancel_rate_limit)
          .kv("current_cancel_rate", r.current_cancel_rate)
          .kv("max_daily_loss",      r.max_daily_loss)
          .kv("current_daily_loss",  r.current_daily_loss)
          .kv("margin_usage_ratio",  r.margin_usage_ratio)
          .kv("last_reject_reason",  r.last_reject_reason)
          .kv("total_rejects_today", r.total_rejects_today)
          .end_object();
        set_json(res, jb.build());
    });

    // POST /api/risk/mode  (only if EnableControl=1)
    server_->Post("/api/risk/mode", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!enable_control_) {
            res.status = 403;
            set_json(res, R"({"error":"control disabled"})");
            return;
        }
        if (!engine_) { res.status = 500; set_json(res, R"({"error":"no engine"})"); return; }

        int mode = 0;
        std::string reason = "webui";
        if (req.has_param("mode")) {
            try { mode = std::stoi(req.get_param_value("mode")); } catch (...) {}
        }
        if (req.has_param("reason"))
            reason = req.get_param_value("reason");

        engine_->set_risk_mode(static_cast<RiskMode>(mode), reason);
        JsonBuilder jb;
        jb.begin_object()
          .kv("ok",   true)
          .kv("mode", risk_mode_str(static_cast<RiskMode>(mode)))
          .end_object();
        set_json(res, jb.build());
    });

    // POST /api/cancel_order — cancel a pending order
    server_->Post("/api/cancel_order", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!engine_) { res.status = 500; set_json(res, R"({"error":"no engine"})"); return; }
        std::string order_ref = req.has_param("order_ref") ? req.get_param_value("order_ref") : "";
        std::string account_id = req.has_param("account_id") ? req.get_param_value("account_id") : "";
        if (order_ref.empty()) { res.status = 400; set_json(res, R"({"error":"order_ref required"})"); return; }
        bool ok = engine_->cancel_order(order_ref, account_id);
        JsonBuilder jb;
        jb.begin_object().kv("ok", ok).kv("order_ref", order_ref).end_object();
        set_json(res, jb.build());
    });

    // POST /api/cancel_all — cancel all pending orders
    server_->Post("/api/cancel_all", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!engine_) { res.status = 500; set_json(res, R"({"error":"no engine"})"); return; }
        std::string account_id = req.has_param("account_id") ? req.get_param_value("account_id") : "";
        engine_->cancel_all_orders(account_id);
        JsonBuilder jb;
        jb.begin_object().kv("ok", true).end_object();
        set_json(res, jb.build());
    });

    // POST /api/test_order — latency test: place a limit order and measure round-trip
    server_->Post("/api/test_order", [this, set_json](const httplib::Request& req, httplib::Response& res) {
        if (!engine_) { res.status = 500; set_json(res, R"({"error":"no engine"})"); return; }

        std::string instrument = "IF2507";
        std::string dir_str = "buy";
        double price = 0;
        int volume = 1;
        std::string offset_str = "open";
        if (req.has_param("instrument")) instrument = req.get_param_value("instrument");
        if (req.has_param("direction"))  dir_str = req.get_param_value("direction");
        if (req.has_param("price"))      try { price = std::stod(req.get_param_value("price")); } catch (...) {}
        if (req.has_param("volume"))     try { volume = std::stoi(req.get_param_value("volume")); } catch (...) {}
        if (req.has_param("offset"))     offset_str = req.get_param_value("offset");

        OrderRequest order{};
        safe_copy(order.instrument_id, instrument.c_str(), sizeof(order.instrument_id));
        safe_copy(order.exchange_id, get_exchange_id(instrument.c_str()), sizeof(order.exchange_id));
        order.direction = (dir_str == "sell") ? Direction::Sell : Direction::Buy;
        order.offset = Offset::Open;
        if (offset_str == "close") order.offset = Offset::Close;
        if (offset_str == "close_today") order.offset = Offset::CloseToday;
        order.price = price;
        order.volume = volume;

        auto t0 = std::chrono::steady_clock::now();
        std::string order_ref;
        engine_->send_order(order, order_ref);
        auto t1 = std::chrono::steady_clock::now();
        int64_t send_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        auto lat = engine_->get_latency_snapshot();
        JsonBuilder jb;
        jb.begin_object()
          .kv("ok", true)
          .kv("order_ref", order_ref)
          .kv("instrument", instrument)
          .kv("direction", dir_str)
          .kv("price", price)
          .kv("volume", volume)
          .kv("send_order_us", send_us)
          .kv("tick_to_signal_us", lat.tick_to_signal_us)
          .kv("signal_to_order_us", lat.signal_to_order_us)
          .kv("order_to_trade_us", lat.order_to_trade_us)
          .kv("tick_process_us", lat.tick_process_us)
          .kv("order_process_us", lat.order_process_us)
          .kv("trade_process_us", lat.trade_process_us)
          .end_object();
        set_json(res, jb.build());
    });
}

// ============================================
// Static routes (embedded HTML SPA)
// ============================================

void WebServer::register_static_routes() {
    server_->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(EMBEDDED_HTML, "text/html; charset=utf-8");
    });
}

} // namespace hft

#endif // ENABLE_METRICS
