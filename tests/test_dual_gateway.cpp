// ============================================
// test_dual_gateway.cpp - Dual-active gateway failover tests
// ============================================

#include "gateway/dual_md_gateway.h"
#include "gateway/dual_trade_gateway.h"

#include <stdexcept>
#include <string>
#include <vector>

#define TEST_ASSERT(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            throw std::runtime_error(std::string("ASSERT FAILED: ") + #cond +  \
                                     " at " + __FILE__ + ":" +                 \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_EQ(a, b)                                                  \
    do {                                                                       \
        if ((a) != (b)) {                                                      \
            throw std::runtime_error(std::string("ASSERT_EQ FAILED: ") +       \
                                     #a " != " #b " at " + __FILE__ + ":" +   \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

namespace {

class FakeMdGateway : public hft::IMdGateway {
public:
    explicit FakeMdGateway(const std::string& id) : id_(id) {}

    void init(const hft::Config&, const std::string&, hft::TradingEngine*) override {
        status_ = hft::MdGatewayStatus::LoggedIn;
    }
    void subscribe(const std::vector<std::string>& inst) override { subscribed_ = inst; }
    void unsubscribe(const std::vector<std::string>&) override { subscribed_.clear(); }
    void stop() override { status_ = hft::MdGatewayStatus::Disconnected; }
    bool is_logged_in() const override { return status_ == hft::MdGatewayStatus::LoggedIn; }
    bool wait_for_login(int) override { return is_logged_in(); }
    hft::MdGatewayStatus status() const override { return status_; }

    void simulate_disconnect() {
        auto old = status_;
        status_ = hft::MdGatewayStatus::Disconnected;
        notify_status_change(old, status_);
    }

    void simulate_reconnect() {
        auto old = status_;
        status_ = hft::MdGatewayStatus::LoggedIn;
        notify_status_change(old, status_);
    }

    const std::string& id() const { return id_; }
    const std::vector<std::string>& subscribed() const { return subscribed_; }

private:
    std::string id_;
    hft::MdGatewayStatus status_ = hft::MdGatewayStatus::Disconnected;
    std::vector<std::string> subscribed_;
};

class FakeTradeGateway : public hft::ITradeGateway {
public:
    explicit FakeTradeGateway(const std::string& id) : id_(id) {}

    void init(const hft::Config&, const std::string&, hft::TradingEngine*, const std::string& acct) override {
        account_id_ = acct;
        logged_in_ = true;
    }
    void stop() override { logged_in_ = false; }
    bool wait_for_login(int) override { return logged_in_; }
    bool is_logged_in() const override { return logged_in_; }
    int send_order(const hft::OrderRequest&, const std::string&) override { ++order_count_; return 0; }
    int cancel_order(const std::string&, const std::string&, const std::string&, int, int) override { return 0; }
    int query_account() override { return 0; }
    int query_position(const std::string&) override { return 0; }
    int query_active_orders() override { return 0; }
    std::vector<std::string> query_instruments(int) override { return {}; }
    int get_front_id() const override { return id_ == "primary" ? 1 : 2; }
    int get_session_id() const override { return id_ == "primary" ? 100 : 200; }
    int get_max_order_ref() const override { return 0; }

    const std::string& id() const { return id_; }
    int order_count() const { return order_count_; }

private:
    std::string id_;
    bool logged_in_ = false;
    int order_count_ = 0;
};

} // anonymous namespace

void test_dual_md_failover() {
    auto primary = std::make_unique<FakeMdGateway>("primary");
    auto backup = std::make_unique<FakeMdGateway>("backup");
    auto* primary_ptr = primary.get();
    auto* backup_ptr = backup.get();

    hft::DualMdGateway dual;
    hft::Config cfg;
    cfg.set_string("Gateway", "Type", "CTP");
    cfg.set_string("CTP.Account1", "MarketFront", "tcp://primary:1234");
    cfg.set_string("CTP.Account1", "MarketFrontBackup", "tcp://backup:5678");

    int factory_calls = 0;
    dual.set_gateway_factory([&](const std::string&) -> std::unique_ptr<hft::IMdGateway> {
        if (factory_calls++ == 0) return std::move(primary);
        return std::move(backup);
    });

    dual.init(cfg, "CTP.Account1", nullptr);
    TEST_ASSERT(dual.is_logged_in());

    dual.subscribe({"IF2506", "IC2506"});
    TEST_ASSERT_EQ(primary_ptr->subscribed().size(), size_t(2));

    // Simulate primary disconnect -> failover to backup
    primary_ptr->simulate_disconnect();
    TEST_ASSERT_EQ(backup_ptr->subscribed().size(), size_t(2));

    // Simulate primary reconnect -> failback
    primary_ptr->simulate_reconnect();
    TEST_ASSERT_EQ(primary_ptr->subscribed().size(), size_t(2));
}

void test_dual_trade_failover() {
    auto primary = std::make_unique<FakeTradeGateway>("primary");
    auto backup = std::make_unique<FakeTradeGateway>("backup");
    auto* primary_ptr = primary.get();
    auto* backup_ptr = backup.get();

    hft::DualTradeGateway dual;
    dual.set_primary(std::move(primary));
    dual.set_backup(std::move(backup));

    hft::Config cfg;
    cfg.set_string("CTP.Account1", "TradeFront", "tcp://primary:1234");
    cfg.set_string("CTP.Account1", "TradeFrontBackup", "tcp://backup:5678");
    dual.init(cfg, "CTP.Account1", nullptr, "test_account");

    TEST_ASSERT(dual.is_logged_in());
    TEST_ASSERT(dual.is_using_primary());
    TEST_ASSERT_EQ(dual.get_front_id(), 1);

    hft::OrderRequest req{};
    dual.send_order(req, "001");
    TEST_ASSERT_EQ(primary_ptr->order_count(), 1);
    TEST_ASSERT_EQ(backup_ptr->order_count(), 0);

    // Failover
    dual.failover();
    TEST_ASSERT(!dual.is_using_primary());
    TEST_ASSERT_EQ(dual.get_front_id(), 2);
    dual.send_order(req, "002");
    TEST_ASSERT_EQ(backup_ptr->order_count(), 1);

    // Failback
    dual.failback();
    TEST_ASSERT(dual.is_using_primary());
    dual.send_order(req, "003");
    TEST_ASSERT_EQ(primary_ptr->order_count(), 2);
}

void test_dual_md_no_backup() {
    auto primary = std::make_unique<FakeMdGateway>("primary");
    auto* primary_ptr = primary.get();

    hft::DualMdGateway dual;
    hft::Config cfg;
    cfg.set_string("Gateway", "Type", "CTP");
    cfg.set_string("CTP.Account1", "MarketFront", "tcp://primary:1234");

    int factory_calls = 0;
    dual.set_gateway_factory([&](const std::string&) -> std::unique_ptr<hft::IMdGateway> {
        if (factory_calls++ == 0) return std::move(primary);
        return nullptr;
    });

    dual.init(cfg, "CTP.Account1", nullptr);
    TEST_ASSERT(dual.is_logged_in());

    dual.subscribe({"IF2506"});
    TEST_ASSERT_EQ(primary_ptr->subscribed().size(), size_t(1));

    // Disconnect without backup — no crash
    primary_ptr->simulate_disconnect();
    TEST_ASSERT(!dual.is_logged_in());
}
