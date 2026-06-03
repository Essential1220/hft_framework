// test_engine_extreme_modes.cpp - Engine extreme mode regression tests (引擎极端模式回归测试)
// Tests: MD queue overflow, production HFT config, trade/command queue overflow halting
// 测试: 行情队列溢出, 生产 HFT 配置, 交易/指令队列溢出暂停

#include "engine/trading_engine.h"
#include "fakes/fake_trade_gateway.h"

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

using namespace hft;

namespace {

std::filesystem::path write_engine_config(const std::string& filename,
                                          const std::string& extra_performance = "") {
    const auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream ofs(path);
    ofs << "[Accounts]\n"
        << "List = template\n"
        << "MarketDataAccount = template\n"
        << "template.Gateway = CTP\n\n"
        << "[CTP.template]\n"
        << "BrokerID = 9999\n"
        << "UserID =\n"
        << "Password =\n"
        << "AppID = simnow_client_test\n"
        << "AuthCode = 0000000000000000\n"
        << "TradeFront = tcp://127.0.0.1:1\n"
        << "MarketFront = tcp://127.0.0.1:1\n\n"
        << "[Strategies]\n"
        << "List = prod_test\n\n"
        << "[Strategy.prod_test]\n"
        << "Type = simple\n"
        << "Instruments = rb2510\n"
        << "OrderSize = 1\n"
        << "MomentumTicks = 3\n"
        << "CooldownSeconds = 5\n\n"
        << "[Risk]\n"
        << "MaxOrderSize = 5\n"
        << "MaxNetPosition = 10\n"
        << "MaxOrdersPerMinute = 30\n\n"
        << "[Performance]\n"
        << extra_performance
        << "\n[Runtime]\n"
        << "StateFile = runtime_state_test.dat\n";
    return path;
}

std::unique_ptr<TradingEngine> make_engine_with_fake_gateway() {
    auto engine = std::make_unique<TradingEngine>();
    engine->get_account_manager().register_gateway_factory([](const std::string&) {
        return std::make_unique<FakeTradeGateway>();
    });
    return engine;
}

} // namespace

void test_engine_md_queue_overflow_is_reported() {
    auto engine = std::make_unique<TradingEngine>();
    TickData tick{};
    safe_copy(tick.instrument_id, "rb2510", sizeof(tick.instrument_id));
    tick.last_price = 3500.0;

    for (size_t i = 0; i < 70000; ++i) {
        engine->on_tick(tick);
    }

    TEST_ASSERT(engine->has_md_queue_overflow());
    TEST_ASSERT(engine->md_queue_drop_count() > 0);
}

void test_engine_production_hft_config() {
    const auto path = write_engine_config("hft_production_mode_test.ini",
                                          "ProductionHftMode = 1\nMdBatchSize = 512\n");

    auto engine = make_engine_with_fake_gateway();
    TEST_ASSERT(engine->init(path.string()));
    TEST_ASSERT(engine->is_production_hft_mode());
    std::filesystem::remove(path);
}

void test_engine_trade_queue_overflow_halts_trading() {
    const auto path = write_engine_config("hft_trade_queue_overflow_test.ini");

    auto engine = make_engine_with_fake_gateway();
    TEST_ASSERT(engine->init(path.string()));

    OrderInfo order{};
    safe_copy(order.account_id, "template", sizeof(order.account_id));
    safe_copy(order.instrument_id, "rb2510", sizeof(order.instrument_id));
    safe_copy(order.order_ref, "overflow_ref", sizeof(order.order_ref));
    order.status = OrderStatus::Pending;

    for (size_t i = 0; i < AccountContext::kQueueSize + 8; ++i) {
        engine->on_order(order);
    }

    TEST_ASSERT(engine->has_trade_queue_overflow());
    TEST_ASSERT_EQ(engine->get_strategy_state(), StrategyState::Paused);
    TEST_ASSERT_EQ(engine->get_risk_mode("template"), RiskMode::Halted);
    std::filesystem::remove(path);
}

void test_engine_command_queue_overflow_fallback_executes_control_command() {
    const auto path = write_engine_config("hft_command_queue_overflow_test.ini");

    auto engine = make_engine_with_fake_gateway();
    TEST_ASSERT(engine->init(path.string()));

    ConditionalOrder order{};
    safe_copy(order.account_id, "template", sizeof(order.account_id));
    safe_copy(order.instrument_id, "rb2510", sizeof(order.instrument_id));
    order.type = ConditionType::StopLoss;
    order.direction = Direction::Buy;
    order.trigger_price = 3500.0;
    order.order_price = 3500.0;
    order.volume = 1;

    for (size_t i = 0; i < 5000; ++i) {
        engine->add_conditional_order_async(order);
    }

    engine->pause_strategy();

    TEST_ASSERT(engine->has_command_queue_overflow());
    TEST_ASSERT_EQ(engine->get_strategy_state(), StrategyState::Paused);
    std::filesystem::remove(path);
}
