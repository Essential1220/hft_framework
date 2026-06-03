// test_strategy_lifecycle.cpp - Per-strategy lifecycle state regression tests (策略级生命周期状态回归测试)

#include "engine/trading_engine.h"
#include "fakes/fake_trade_gateway.h"
#include "strategy/strategy_base.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

extern void TEST_SUITE(const char*);
#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond \
        + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while (0)
#define TEST_ASSERT_EQ(a, b)                                                  \
    do { if ((a) != (b)) throw std::runtime_error(std::string("EQ: ") + #a +   \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while (0)
#define TEST_ASSERT_NEAR(a, b, eps)                                           \
    do { if (std::abs((a) - (b)) > (eps)) throw std::runtime_error(std::string("NEAR: ") + #a +   \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while (0)

using namespace hft;

namespace {

class DummyStrategy : public StrategyBase {
public:
    explicit DummyStrategy(std::string id) {
        strategy_id_ = std::move(id);
        strategy_type_ = "dummy";
    }

    void on_init() override {}
    void on_tick(const TickData&) override {}
    void on_order(const OrderInfo&) override {}
    void on_trade(const TradeInfo&) override {}
};

void copy_text(char* dst, size_t size, const char* src) {
    std::strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

std::filesystem::path write_strategy_lifecycle_config() {
    const auto path = std::filesystem::temp_directory_path() / "hft_strategy_runtime_stats_test.ini";
    std::ofstream ofs(path);
    ofs << "[Accounts]\n"
        << "List = acct1\n"
        << "MarketDataAccount = acct1\n"
        << "acct1.Gateway = CTP\n\n"
        << "[CTP.acct1]\n"
        << "BrokerID = 9999\n"
        << "UserID = acct1\n"
        << "Password =\n"
        << "AppID = simnow_client_test\n"
        << "AuthCode = 0000000000000000\n"
        << "TradeFront = tcp://127.0.0.1:1\n"
        << "MarketFront = tcp://127.0.0.1:1\n\n"
        << "[Strategies]\n"
        << "List = alpha\n\n"
        << "[Strategy.alpha]\n"
        << "Type = simple\n"
        << "Instruments = rb2510\n"
        << "OrderSize = 1\n"
        << "MomentumTicks = 3\n"
        << "CooldownSeconds = 5\n\n"
        << "[Risk]\n"
        << "MaxOrderSize = 5\n"
        << "MaxNetPosition = 10\n"
        << "MaxOrdersPerMinute = 30\n\n"
        << "[Runtime]\n"
        << "StateFile = runtime_state_strategy_stats_test.dat\n";
    return path;
}

} // namespace

void test_strategy_lifecycle_is_per_strategy() {
    auto engine = std::make_unique<TradingEngine>();
    auto strategy = std::make_shared<DummyStrategy>("alpha");

    TEST_ASSERT(engine->add_strategy(strategy));
    TEST_ASSERT_EQ(engine->get_strategy_state("alpha"), StrategyState::Running);

    TEST_ASSERT(engine->set_strategy_state("alpha", StrategyState::Paused));
    auto snapshots = engine->get_strategy_snapshots();
    TEST_ASSERT_EQ(snapshots.size(), 1u);
    TEST_ASSERT_EQ(snapshots[0].strategy_id, "alpha");
    TEST_ASSERT_EQ(snapshots[0].status, "paused");

    TEST_ASSERT(engine->set_strategy_state("alpha", StrategyState::Stopped));
    snapshots = engine->get_strategy_snapshots();
    TEST_ASSERT_EQ(snapshots[0].status, "stopped");

    TEST_ASSERT(engine->set_strategy_state("alpha", StrategyState::Running));
    snapshots = engine->get_strategy_snapshots();
    TEST_ASSERT_EQ(snapshots[0].status, "running");

    TEST_ASSERT(!engine->set_strategy_state("ghost", StrategyState::Paused));

    TEST_ASSERT(engine->remove_strategy("alpha"));
    TEST_ASSERT_EQ(engine->get_strategy_snapshots().size(), 0u);
}

void test_strategy_snapshot_includes_runtime_position_stats() {
    auto engine = std::make_unique<TradingEngine>();
    engine->get_account_manager().register_gateway_factory([](const std::string&) {
        return std::make_unique<FakeTradeGateway>();
    });
    const auto config_path = write_strategy_lifecycle_config();
    TEST_ASSERT(engine->init(config_path.string()));

    auto strategy = std::make_shared<DummyStrategy>("alpha");
    strategy->configure_context("alpha", "acct1", {"rb2510"});
    TEST_ASSERT(engine->add_strategy(strategy));

    PositionInfo pos{};
    copy_text(pos.account_id, sizeof(pos.account_id), "acct1");
    copy_text(pos.instrument_id, sizeof(pos.instrument_id), "rb2510");
    pos.direction = Direction::Buy;
    pos.total = 3;
    pos.avg_price = 3501.5;
    engine->apply_position_snapshot("acct1", {pos});

    auto snapshots = engine->get_strategy_snapshots("acct1");
    TEST_ASSERT_EQ(snapshots.size(), 1u);
    TEST_ASSERT_EQ(snapshots[0].position_count, 1u);
    TEST_ASSERT_EQ(snapshots[0].position_volume, 3);
    TEST_ASSERT_NEAR(snapshots[0].avg_price, 3501.5, 0.01);
    std::filesystem::remove(config_path);
}
