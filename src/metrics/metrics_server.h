#pragma once
// ============================================
// metrics_server.h - Prometheus /metrics HTTP endpoint (Prometheus 指标 HTTP 端点)
// Exposes engine telemetry in Prometheus text exposition format via cpp-httplib.
// (通过 cpp-httplib 以 Prometheus text exposition 格式暴露引擎遥测数据)
// ============================================

#ifdef ENABLE_METRICS

#include <memory>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace hft {

class TradingEngine;

class MetricsServer {
public:
    MetricsServer();
    ~MetricsServer();

    void start(int port, TradingEngine* engine);
    void stop();

private:
    std::string render_metrics() const;

    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    TradingEngine* engine_ = nullptr;
    int port_ = 0;
};

} // namespace hft

#endif // ENABLE_METRICS
