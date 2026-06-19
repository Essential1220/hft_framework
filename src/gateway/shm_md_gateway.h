#pragma once
// ============================================
// shm_md_gateway.h - Shared-memory market data gateway
// Reads TickData from a ShmQueue written by an external producer
// process (e.g. a standalone CTP gateway), then dispatches to
// TradingEngine::on_tick().
// ============================================

#include "gateway/i_md_gateway.h"
#include "common/shm_queue.h"
#include "common/types.h"

#include <atomic>
#include <thread>

namespace hft {

class TradingEngine;
class Config;

class ShmMdGateway : public IMdGateway {
public:
    static constexpr size_t kQueueCapacity = 65536;  // 64K slots

    ShmMdGateway() = default;
    ~ShmMdGateway() override;

    void init(const Config& config, const std::string& section,
              TradingEngine* engine) override;
    void subscribe(const std::vector<std::string>& instruments) override;
    void unsubscribe(const std::vector<std::string>& instruments) override;
    void stop() override;
    bool is_logged_in() const override;
    bool wait_for_login(int timeout_sec = 10) override;
    MdGatewayStatus status() const override;

private:
    void poll_loop();

    TradingEngine* engine_ = nullptr;
    ShmQueue<TickData, kQueueCapacity> shm_queue_;
    std::thread poll_thread_;
    std::atomic<bool> running_{false};
    std::atomic<MdGatewayStatus> status_{MdGatewayStatus::Disconnected};
    std::string queue_name_;
};

} // namespace hft
