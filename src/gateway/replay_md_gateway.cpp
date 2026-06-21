#include "gateway/replay_md_gateway.h"
#include "engine/trading_engine.h"
#include "common/logger.h"

#include <algorithm>
#include <thread>

namespace hft {

ReplayMdGateway::ReplayMdGateway(std::vector<TickData> ticks)
    : ticks_(std::move(ticks)) {}

void ReplayMdGateway::init(const Config&, const std::string&, TradingEngine* engine) {
    engine_ = engine;
}

void ReplayMdGateway::subscribe(const std::vector<std::string>& instruments) {
    for (const auto& inst : instruments) {
        subscriptions_.insert(inst);
    }
}

void ReplayMdGateway::unsubscribe(const std::vector<std::string>& instruments) {
    for (const auto& inst : instruments) {
        subscriptions_.erase(inst);
    }
}

void ReplayMdGateway::stop() { stopped_ = true; }
bool ReplayMdGateway::is_logged_in() const { return !stopped_; }
bool ReplayMdGateway::wait_for_login(int) { return true; }

MdGatewayStatus ReplayMdGateway::status() const {
    return stopped_ ? MdGatewayStatus::Disconnected : MdGatewayStatus::LoggedIn;
}

size_t ReplayMdGateway::replay() {
    if (!engine_) return 0;

    size_t fed = 0;
    for (auto& tick : ticks_) {
        if (stopped_) break;

        if (!subscriptions_.empty() &&
            subscriptions_.find(tick.instrument_id) == subscriptions_.end()) {
            continue;
        }

        engine_->on_tick(tick);
        ++fed;
    }

    // Allow consumer thread to drain the queue
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    LOG_INFO("ReplayMdGateway: fed " + std::to_string(fed) + " ticks");
    return fed;
}

} // namespace hft
