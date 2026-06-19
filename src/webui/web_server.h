#pragma once
// ============================================
// web_server.h - HTTP server: Prometheus /metrics + REST API + WebUI
// Supersedes MetricsServer. Guarded by ENABLE_METRICS.
// ============================================

#ifdef ENABLE_METRICS

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace hft {

class TradingEngine;

class WebServer {
public:
    WebServer();
    ~WebServer();

    void start(int port, TradingEngine* engine, bool enable_control = false);
    void stop();

private:
    void register_metrics_routes();
    void register_api_routes();
    void register_static_routes();

    std::string render_metrics() const;

    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    TradingEngine* engine_ = nullptr;
    int port_ = 0;
    bool enable_control_ = false;
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace hft

#endif // ENABLE_METRICS
