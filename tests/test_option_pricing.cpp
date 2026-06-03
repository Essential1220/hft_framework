// test_option_pricing.cpp - Option pricing unit tests (期权定价单元测试)
// Tests: Black-Scholes price, implied volatility, Greeks (delta, gamma, vega, theta)
// 测试: Black-Scholes 定价, 隐含波动率, Greeks (delta/gamma/vega/theta)

#include "market/option_pricing.h"

#include <stdexcept>
#include <string>

#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond); } while (0)

#define TEST_ASSERT_NEAR(a, b, eps)                                           \
    do {                                                                      \
        const double _diff = ((a) > (b)) ? ((a) - (b)) : ((b) - (a));          \
        if (_diff > (eps)) throw std::runtime_error(std::string("ASSERT_NEAR: ") + #a " vs " #b); \
    } while (0)

using namespace hft;

void test_option_bs_call_price() {
    const double price = black_scholes_price(OptionType::Call, 100.0, 100.0, 1.0, 0.05, 0.20);
    TEST_ASSERT_NEAR(price, 10.4506, 0.001);
}

void test_option_metrics_solves_call_iv_and_greeks() {
    OptionPricingInput input;
    input.type = OptionType::Call;
    input.underlying_price = 100.0;
    input.strike_price = 100.0;
    input.years_to_expiry = 1.0;
    input.risk_free_rate = 0.05;
    input.option_price = black_scholes_price(input.type, input.underlying_price,
                                             input.strike_price, input.years_to_expiry,
                                             input.risk_free_rate, 0.20);

    const auto metrics = calculate_option_metrics(input);
    TEST_ASSERT(metrics.ok);
    TEST_ASSERT_NEAR(metrics.implied_volatility, 0.20, 0.0001);
    TEST_ASSERT_NEAR(metrics.delta, 0.6368, 0.001);
    TEST_ASSERT(metrics.gamma > 0.0);
    TEST_ASSERT(metrics.vega > 0.0);
    TEST_ASSERT(metrics.time_value > 0.0);
}

void test_option_metrics_solves_put_iv_and_delta() {
    OptionPricingInput input;
    input.type = OptionType::Put;
    input.underlying_price = 100.0;
    input.strike_price = 100.0;
    input.years_to_expiry = 1.0;
    input.risk_free_rate = 0.05;
    input.option_price = black_scholes_price(input.type, input.underlying_price,
                                             input.strike_price, input.years_to_expiry,
                                             input.risk_free_rate, 0.25);

    const auto metrics = calculate_option_metrics(input);
    TEST_ASSERT(metrics.ok);
    TEST_ASSERT_NEAR(metrics.implied_volatility, 0.25, 0.0001);
    TEST_ASSERT(metrics.delta < 0.0);
    TEST_ASSERT(metrics.gamma > 0.0);
}

void test_option_metrics_rejects_missing_market_inputs() {
    OptionPricingInput input;
    input.type = OptionType::Call;
    input.option_price = 10.0;
    input.underlying_price = 0.0;
    input.strike_price = 100.0;
    input.years_to_expiry = 1.0;

    const auto metrics = calculate_option_metrics(input);
    TEST_ASSERT(!metrics.ok);
    TEST_ASSERT(metrics.reason == "invalid_underlying_price");
}
