#pragma once
// ============================================
// fake_trade_gateway.h - 用于离线集成测试的模拟交易网关
// ============================================

#include "gateway/i_trade_gateway.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace hft {

class FakeTradeGateway : public ITradeGateway {
public:
    // Record sent order requests (记录发送的订单请求)
    struct SentOrder {
        OrderRequest req;
        std::string order_ref;
    };

    void init(const Config& config, const std::string& section,
              TradingEngine* engine, const std::string& account_id) override {
        engine_ = engine;
        account_id_ = account_id;
        logged_in_ = true;
    }

    void stop() override { logged_in_ = false; }
    bool wait_for_login(int timeout_sec = 30) override { return logged_in_; }
    bool is_logged_in() const override { return logged_in_; }

    int send_order(const OrderRequest& req, const std::string& order_ref) override {
        std::lock_guard<std::mutex> lock(mtx_);
        sent_orders_.push_back({req, order_ref});
        ++send_count_;
        if (send_fn_) return send_fn_(req, order_ref);
        return 0; // 0 = success
    }

    int cancel_order(const std::string& instrument_id, const std::string& exchange_id,
                     const std::string& order_ref, int front_id, int session_id) override {
        std::lock_guard<std::mutex> lock(mtx_);
        cancelled_refs_.push_back(order_ref);
        ++cancel_count_;
        return 0;
    }

    int query_account() override { return 0; }
    int query_position(const std::string& instrument_id = "") override { return 0; }
    int query_active_orders() override { return 0; }
    std::vector<std::string> query_instruments(int timeout_sec = 30) override { return {}; }

    int get_front_id() const override { return 1; }
    int get_session_id() const override { return 100; }
    int get_max_order_ref() const override { return max_order_ref_++; }

    // ---- Test helpers (测试辅助) ----
    void set_send_handler(std::function<int(const OrderRequest&, const std::string&)> fn) {
        std::lock_guard<std::mutex> lock(mtx_);
        send_fn_ = std::move(fn);
    }

    std::vector<SentOrder> get_sent_orders() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return sent_orders_;
    }

    std::vector<std::string> get_cancelled_refs() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return cancelled_refs_;
    }

    int get_send_count() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return send_count_;
    }

    int get_cancel_count() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return cancel_count_;
    }

    void clear_history() {
        std::lock_guard<std::mutex> lock(mtx_);
        sent_orders_.clear();
        cancelled_refs_.clear();
        send_count_ = 0;
        cancel_count_ = 0;
    }

    TradingEngine* engine() const { return engine_; }

private:
    TradingEngine* engine_ = nullptr;
    bool logged_in_ = false;
    mutable std::mutex mtx_;
    std::vector<SentOrder> sent_orders_;
    std::vector<std::string> cancelled_refs_;
    int send_count_ = 0;
    int cancel_count_ = 0;
    mutable int max_order_ref_ = 1;
    std::function<int(const OrderRequest&, const std::string&)> send_fn_;
};

} // namespace hft
