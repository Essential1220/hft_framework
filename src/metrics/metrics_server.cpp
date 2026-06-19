// ============================================
// metrics_server.cpp - Prometheus /metrics HTTP endpoint implementation
// ============================================

#ifdef ENABLE_METRICS

#include "metrics/metrics_server.h"
#include "engine/trading_engine.h"
#include "common/logger.h"

#include <httplib.h>
#include <sstream>

namespace hft {

MetricsServer::MetricsServer() = default;

MetricsServer::~MetricsServer() {
    stop();
}

void MetricsServer::start(int port, TradingEngine* engine) {
    if (server_) return;
    engine_ = engine;
    port_ = port;
    server_ = std::make_unique<httplib::Server>();

    server_->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(render_metrics(), "text/plain; version=0.0.4; charset=utf-8");
    });

    server_->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });

    server_thread_ = std::thread([this]() {
        LOG_INFO("MetricsServer listening on port " + std::to_string(port_));
        server_->listen("0.0.0.0", port_);
    });
}

void MetricsServer::stop() {
    if (server_) {
        server_->stop();
        if (server_thread_.joinable())
            server_thread_.join();
        server_.reset();
        LOG_INFO("MetricsServer stopped");
    }
}

static std::string escape_label(const std::string& v) {
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

std::string MetricsServer::render_metrics() const {
    if (!engine_) return "";
    std::ostringstream ss;

    // --- Latency ---
    auto lat = engine_->get_latency_snapshot();
    ss << "# HELP hft_tick_to_signal_us Last tick-to-signal latency in microseconds\n"
       << "# TYPE hft_tick_to_signal_us gauge\n"
       << "hft_tick_to_signal_us " << lat.tick_to_signal_us << "\n\n";

    ss << "# HELP hft_signal_to_order_us Last signal-to-order latency in microseconds\n"
       << "# TYPE hft_signal_to_order_us gauge\n"
       << "hft_signal_to_order_us " << lat.signal_to_order_us << "\n\n";

    ss << "# HELP hft_order_to_trade_us Last order-to-trade latency in microseconds\n"
       << "# TYPE hft_order_to_trade_us gauge\n"
       << "hft_order_to_trade_us " << lat.order_to_trade_us << "\n\n";

    ss << "# HELP hft_tick_process_us Last tick processing latency in microseconds\n"
       << "# TYPE hft_tick_process_us gauge\n"
       << "hft_tick_process_us " << lat.tick_process_us << "\n\n";

    // --- Queue ---
    ss << "# HELP hft_md_queue_drops_total Total MD queue drops\n"
       << "# TYPE hft_md_queue_drops_total counter\n"
       << "hft_md_queue_drops_total " << engine_->md_queue_drop_count() << "\n\n";

    ss << "# HELP hft_md_queue_overflow Queue overflow flag (1=overflow detected)\n"
       << "# TYPE hft_md_queue_overflow gauge\n"
       << "hft_md_queue_overflow " << (engine_->has_md_queue_overflow() ? 1 : 0) << "\n\n";

    // --- Risk mode ---
    ss << "# HELP hft_risk_mode Current RMS risk mode (0=Normal..5=Halted)\n"
       << "# TYPE hft_risk_mode gauge\n"
       << "hft_risk_mode " << static_cast<int>(engine_->get_risk_mode()) << "\n\n";

    // --- Account metrics ---
    auto accounts = engine_->get_account_snapshots();
    if (!accounts.empty()) {
        ss << "# HELP hft_account_balance Account balance\n"
           << "# TYPE hft_account_balance gauge\n";
        for (const auto& a : accounts) {
            auto aid = escape_label(a.account_id);
            ss << "hft_account_balance{account=\"" << aid << "\"} " << a.account.balance << "\n";
        }
        ss << "\n";

        ss << "# HELP hft_account_available Account available funds\n"
           << "# TYPE hft_account_available gauge\n";
        for (const auto& a : accounts) {
            auto aid = escape_label(a.account_id);
            ss << "hft_account_available{account=\"" << aid << "\"} " << a.account.available << "\n";
        }
        ss << "\n";

        ss << "# HELP hft_account_margin Account margin used\n"
           << "# TYPE hft_account_margin gauge\n";
        for (const auto& a : accounts) {
            auto aid = escape_label(a.account_id);
            ss << "hft_account_margin{account=\"" << aid << "\"} " << a.account.margin << "\n";
        }
        ss << "\n";

        ss << "# HELP hft_account_position_profit Account position (floating) profit\n"
           << "# TYPE hft_account_position_profit gauge\n";
        for (const auto& a : accounts) {
            auto aid = escape_label(a.account_id);
            ss << "hft_account_position_profit{account=\"" << aid << "\"} " << a.account.position_profit << "\n";
        }
        ss << "\n";
    }

    // --- Strategy performance ---
    auto strategies = engine_->get_strategy_performance();
    if (!strategies.empty()) {
        ss << "# HELP hft_strategy_trades_total Per-strategy trade count\n"
           << "# TYPE hft_strategy_trades_total counter\n";
        for (const auto& s : strategies) {
            auto sid = escape_label(s.strategy_id);
            ss << "hft_strategy_trades_total{strategy=\"" << sid << "\"} " << s.trade_count << "\n";
        }
        ss << "\n";

        ss << "# HELP hft_strategy_pnl Per-strategy total P&L\n"
           << "# TYPE hft_strategy_pnl gauge\n";
        for (const auto& s : strategies) {
            auto sid = escape_label(s.strategy_id);
            ss << "hft_strategy_pnl{strategy=\"" << sid << "\"} " << s.total_pnl << "\n";
        }
        ss << "\n";

        ss << "# HELP hft_strategy_win_rate Per-strategy win rate\n"
           << "# TYPE hft_strategy_win_rate gauge\n";
        for (const auto& s : strategies) {
            auto sid = escape_label(s.strategy_id);
            ss << "hft_strategy_win_rate{strategy=\"" << sid << "\"} " << s.win_rate << "\n";
        }
        ss << "\n";
    }

    // --- PnL ---
    auto pnl = engine_->get_pnl_curve(1);
    if (!pnl.empty()) {
        ss << "# HELP hft_daily_pnl Current daily P&L\n"
           << "# TYPE hft_daily_pnl gauge\n"
           << "hft_daily_pnl " << pnl.back().total_pnl << "\n\n";
    }

    // --- Engine state ---
    ss << "# HELP hft_engine_running Whether engine is running (1=yes)\n"
       << "# TYPE hft_engine_running gauge\n"
       << "hft_engine_running " << (engine_->is_running() ? 1 : 0) << "\n\n";

    ss << "# HELP hft_trading_ready Whether trading is ready (1=yes)\n"
       << "# TYPE hft_trading_ready gauge\n"
       << "hft_trading_ready " << (engine_->is_trading_ready() ? 1 : 0) << "\n\n";

    return ss.str();
}

} // namespace hft

#endif // ENABLE_METRICS
