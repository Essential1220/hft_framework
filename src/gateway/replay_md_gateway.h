#pragma once

#include "gateway/i_md_gateway.h"
#include "common/types.h"

#include <string>
#include <vector>
#include <set>

namespace hft {

class TradingEngine;

class ReplayMdGateway : public IMdGateway {
public:
    explicit ReplayMdGateway(std::vector<TickData> ticks);

    void init(const Config& config, const std::string& section, TradingEngine* engine) override;
    void subscribe(const std::vector<std::string>& instruments) override;
    void unsubscribe(const std::vector<std::string>& instruments) override;
    void stop() override;
    bool is_logged_in() const override;
    bool wait_for_login(int timeout_sec = 10) override;
    MdGatewayStatus status() const override;

    // Run the replay synchronously — feeds all ticks to engine, returns tick count
    size_t replay();

    size_t total_ticks() const { return ticks_.size(); }

private:
    TradingEngine* engine_ = nullptr;
    std::vector<TickData> ticks_;
    std::set<std::string> subscriptions_;
    bool stopped_ = false;
};

} // namespace hft
