#pragma once
// ============================================
// fake_md_gateway.h - 用于离线集成测试的模拟行情网关
// ============================================

#include "gateway/i_md_gateway.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace hft {

class FakeMdGateway : public IMdGateway {
public:
    void init(const Config& config, const std::string& section, TradingEngine* engine) override {
        engine_ = engine;
        logged_in_ = true;
    }

    void subscribe(const std::vector<std::string>& instruments) override {
        std::lock_guard<std::mutex> lock(mtx_);
        subscriptions_.insert(subscriptions_.end(), instruments.begin(), instruments.end());
    }

    void unsubscribe(const std::vector<std::string>& instruments) override {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& inst : instruments) {
            subscriptions_.erase(
                std::remove(subscriptions_.begin(), subscriptions_.end(), inst),
                subscriptions_.end());
        }
    }

    void stop() override { logged_in_ = false; }
    bool is_logged_in() const override { return logged_in_; }
    bool wait_for_login(int timeout_sec = 10) override { return logged_in_; }
    MdGatewayStatus status() const override { return logged_in_ ? MdGatewayStatus::LoggedIn : MdGatewayStatus::Disconnected; }

    // ---- Test helpers (测试辅助) ----
    std::vector<std::string> get_subscriptions() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return subscriptions_;
    }

    TradingEngine* engine() const { return engine_; }

private:
    TradingEngine* engine_ = nullptr;
    bool logged_in_ = false;
    mutable std::mutex mtx_;
    std::vector<std::string> subscriptions_;
};

} // namespace hft
