// ============================================
// shm_md_gateway.cpp - Shared-memory market data gateway
// ============================================

#include "gateway/shm_md_gateway.h"
#include "engine/trading_engine.h"
#include "common/config_store.h"
#include "common/logger.h"

#include <chrono>

namespace hft {

ShmMdGateway::~ShmMdGateway() {
    stop();
}

void ShmMdGateway::init(const Config& config, const std::string& section,
                         TradingEngine* engine) {
    engine_ = engine;
    queue_name_ = config.get_string(section, "ShmQueueName", "hft_md");

    if (!shm_queue_.open(queue_name_)) {
        LOG_ERROR("ShmMdGateway: failed to open shared memory queue '" + queue_name_ + "'");
        status_.store(MdGatewayStatus::Disconnected, std::memory_order_release);
        return;
    }

    LOG_INFO("ShmMdGateway: connected to shared memory queue '" + queue_name_ + "'");
    status_.store(MdGatewayStatus::LoggedIn, std::memory_order_release);
    notify_status_change(MdGatewayStatus::Disconnected, MdGatewayStatus::LoggedIn);

    running_.store(true, std::memory_order_release);
    poll_thread_ = std::thread(&ShmMdGateway::poll_loop, this);
}

void ShmMdGateway::subscribe(const std::vector<std::string>&) {
    // No-op: the external producer decides which instruments to write.
}

void ShmMdGateway::unsubscribe(const std::vector<std::string>&) {
    // No-op.
}

void ShmMdGateway::stop() {
    running_.store(false, std::memory_order_release);
    if (poll_thread_.joinable())
        poll_thread_.join();

    auto old = status_.exchange(MdGatewayStatus::Disconnected, std::memory_order_acq_rel);
    if (old != MdGatewayStatus::Disconnected)
        notify_status_change(old, MdGatewayStatus::Disconnected);

    shm_queue_.close();
    LOG_INFO("ShmMdGateway stopped");
}

bool ShmMdGateway::is_logged_in() const {
    return status_.load(std::memory_order_acquire) == MdGatewayStatus::LoggedIn;
}

bool ShmMdGateway::wait_for_login(int) {
    return is_logged_in();
}

MdGatewayStatus ShmMdGateway::status() const {
    return status_.load(std::memory_order_acquire);
}

void ShmMdGateway::poll_loop() {
    TickData tick;
    while (running_.load(std::memory_order_acquire)) {
        if (shm_queue_.pop(tick)) {
            engine_->on_tick(tick);
        } else {
            std::this_thread::yield();
        }
    }
}

} // namespace hft
