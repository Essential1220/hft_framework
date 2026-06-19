// ============================================
// test_feature_pipeline.cpp - Feature pipeline indicator tests
// SMA/EMA/RSI hand-computed verification
// ============================================

#include "market/feature_pipeline.h"

#include <cmath>
#include <stdexcept>
#include <string>

#define TEST_ASSERT(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            throw std::runtime_error(std::string("ASSERT FAILED: ") + #cond +  \
                                     " at " + __FILE__ + ":" +                 \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_NEAR(a, b, eps)                                           \
    do {                                                                       \
        double _diff = (double)(a) - (double)(b);                             \
        if (_diff < 0) _diff = -_diff;                                        \
        if (_diff > (eps)) {                                                   \
            throw std::runtime_error(std::string("ASSERT_NEAR FAILED: ") +     \
                                     #a " != " #b " (diff=" +                  \
                                     std::to_string(_diff) + ") at " +         \
                                     __FILE__ + ":" + std::to_string(__LINE__));\
        }                                                                      \
    } while (0)

namespace {

hft::TickData make_tick(const char* inst, double price, int volume,
                        double high, double low,
                        int bid_vol = 100, int ask_vol = 100,
                        double turnover = 0.0) {
    hft::TickData t{};
    hft::safe_copy(t.instrument_id, inst, sizeof(t.instrument_id));
    t.last_price = price;
    t.volume = volume;
    t.highest_price = high;
    t.lowest_price = low;
    t.bid[0].price = price - 0.2;
    t.bid[0].volume = bid_vol;
    t.ask[0].price = price + 0.2;
    t.ask[0].volume = ask_vol;
    t.turnover = turnover > 0.0 ? turnover : price * volume;
    return t;
}

} // anonymous namespace

void test_feature_sma() {
    hft::Config cfg;
    cfg.set_string("Features", "Enable", "1");
    cfg.set_string("Features", "SmaPeriod", "5");
    cfg.set_string("Features", "HistorySize", "100");

    hft::FeaturePipeline pipeline;
    pipeline.init(cfg);

    double prices[] = {10.0, 11.0, 12.0, 13.0, 14.0, 15.0};
    for (double p : prices) {
        pipeline.on_tick(make_tick("IF2506", p, 100, p + 1.0, p - 1.0));
    }

    // SMA(5) of last 5 prices: (15+14+13+12+11)/5 = 13.0
    double sma = pipeline.get_feature("IF2506", "sma");
    TEST_ASSERT_NEAR(sma, 13.0, 0.001);
}

void test_feature_ema() {
    hft::Config cfg;
    cfg.set_string("Features", "Enable", "1");
    cfg.set_string("Features", "EmaPeriod", "3");
    cfg.set_string("Features", "HistorySize", "100");

    hft::FeaturePipeline pipeline;
    pipeline.init(cfg);

    // EMA(3): k = 2/(3+1) = 0.5
    // After 3 ticks: EMA = SMA(3) = (10+11+12)/3 = 11.0
    // After 4th tick (13): EMA = 13*0.5 + 11.0*0.5 = 12.0
    // After 5th tick (14): EMA = 14*0.5 + 12.0*0.5 = 13.0
    double prices[] = {10.0, 11.0, 12.0, 13.0, 14.0};
    for (double p : prices) {
        pipeline.on_tick(make_tick("IF2506", p, 100, p + 1.0, p - 1.0));
    }

    double ema = pipeline.get_feature("IF2506", "ema");
    TEST_ASSERT_NEAR(ema, 13.0, 0.001);
}

void test_feature_rsi() {
    hft::Config cfg;
    cfg.set_string("Features", "Enable", "1");
    cfg.set_string("Features", "RsiPeriod", "3");
    cfg.set_string("Features", "HistorySize", "100");

    hft::FeaturePipeline pipeline;
    pipeline.init(cfg);

    // Prices: 10, 12, 11, 14 (4 ticks, RSI period=3 needs 4 prices)
    // Changes (most recent first in ring): 14-11=+3, 11-12=-1, 12-10=+2
    // gains: 3+2=5, losses: 1
    // avg_gain=5/3, avg_loss=1/3, RS=5, RSI=100-100/6=83.33
    double prices[] = {10.0, 12.0, 11.0, 14.0};
    for (double p : prices) {
        pipeline.on_tick(make_tick("IF2506", p, 100, p + 1.0, p - 1.0));
    }

    double rsi = pipeline.get_feature("IF2506", "rsi");
    TEST_ASSERT_NEAR(rsi, 83.333, 0.01);
}

void test_feature_bid_ask_imbalance() {
    hft::Config cfg;
    cfg.set_string("Features", "Enable", "1");

    hft::FeaturePipeline pipeline;
    pipeline.init(cfg);

    // bid_vol=300, ask_vol=100 -> imbalance = (300-100)/400 = 0.5
    pipeline.on_tick(make_tick("IF2506", 100.0, 100, 101.0, 99.0, 300, 100));

    double imb = pipeline.get_feature("IF2506", "bid_ask_imbalance");
    TEST_ASSERT_NEAR(imb, 0.5, 0.001);
}

void test_feature_vwap() {
    hft::Config cfg;
    cfg.set_string("Features", "Enable", "1");

    hft::FeaturePipeline pipeline;
    pipeline.init(cfg);

    // Tick 1: price=10, vol=100, turnover=1000
    // Tick 2: price=20, vol=200, turnover=4000
    // VWAP = (1000+4000)/(100+200) = 5000/300 = 16.667
    pipeline.on_tick(make_tick("IF2506", 10.0, 100, 11.0, 9.0, 100, 100, 1000.0));
    pipeline.on_tick(make_tick("IF2506", 20.0, 200, 21.0, 19.0, 100, 100, 4000.0));

    double vwap = pipeline.get_feature("IF2506", "vwap");
    TEST_ASSERT_NEAR(vwap, 16.667, 0.01);
}

void test_feature_multi_instrument() {
    hft::Config cfg;
    cfg.set_string("Features", "Enable", "1");
    cfg.set_string("Features", "SmaPeriod", "3");

    hft::FeaturePipeline pipeline;
    pipeline.init(cfg);

    // IF2506: prices 10, 20, 30 -> SMA(3)=20
    // IC2506: prices 100, 200, 300 -> SMA(3)=200
    pipeline.on_tick(make_tick("IF2506", 10.0, 100, 11.0, 9.0));
    pipeline.on_tick(make_tick("IC2506", 100.0, 100, 101.0, 99.0));
    pipeline.on_tick(make_tick("IF2506", 20.0, 100, 21.0, 19.0));
    pipeline.on_tick(make_tick("IC2506", 200.0, 100, 201.0, 199.0));
    pipeline.on_tick(make_tick("IF2506", 30.0, 100, 31.0, 29.0));
    pipeline.on_tick(make_tick("IC2506", 300.0, 100, 301.0, 299.0));

    TEST_ASSERT_NEAR(pipeline.get_feature("IF2506", "sma"), 20.0, 0.001);
    TEST_ASSERT_NEAR(pipeline.get_feature("IC2506", "sma"), 200.0, 0.001);
}

void test_feature_disabled() {
    hft::Config cfg;
    cfg.set_string("Features", "Enable", "0");

    hft::FeaturePipeline pipeline;
    pipeline.init(cfg);

    pipeline.on_tick(make_tick("IF2506", 100.0, 100, 101.0, 99.0));
    TEST_ASSERT_NEAR(pipeline.get_feature("IF2506", "sma"), 0.0, 0.001);
    TEST_ASSERT_NEAR(pipeline.get_feature("IF2506", "tick_count"), 0.0, 0.001);
}
