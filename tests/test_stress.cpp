// test_stress.cpp - Stress tests with QPC high-precision measurement (压力测试, QPC 高精度计时)

#include "common/spsc_queue.h"
#include "common/event.h"
#include "common/types.h"
#include "common/qpc_timer.h"
#include "order/conditional_order_manager.h"
#include "order/order_manager.h"
#include "position/position_manager.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

extern void TEST_SUITE(const char*);
#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond \
        + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)
#define TEST_ASSERT_EQ(a, b)                                                  \
    do { if ((a) != (b)) throw std::runtime_error(std::string("EQ: ") + #a +   \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)

using namespace hft;

void test_stress_spsc_throughput() {
    constexpr size_t N = 10000000;
    SPSCQueue<size_t, 65536> q;

    auto start = std::chrono::steady_clock::now();

    std::thread producer([&]() {
        for (size_t i = 0; i < N; ++i) {
            while (!q.push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        size_t val;
        size_t count = 0;
        while (count < N) {
            if (q.pop(val)) {
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    double ops_per_sec = (N * 2.0) / (ms / 1000.0);

    printf("    [info] SPSC throughput: %zu items, %.2f ms, %.0f ops/sec\n",
           N, ms, ops_per_sec);
    TEST_ASSERT(q.empty());
}

void test_stress_cond_order_concurrent() {
    ConditionalOrderManager mgr;
    constexpr int NUM_ORDERS = 10000;
    constexpr int NUM_TICKS = 5000;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < NUM_ORDERS; ++i) {
        ConditionalOrder o{};
        safe_copy(o.instrument_id, "rb2510", sizeof(o.instrument_id));
        o.type = ConditionType::StopLoss;
        o.direction = Direction::Sell;
        o.trigger_price = 3500.0 - i * 0.01;
        o.volume = 1;
        mgr.add(o);
    }

    std::atomic<bool> done{false};
    std::atomic<int> add_count{0};

    std::thread adder([&]() {
        for (int i = 0; i < 5000; ++i) {
            ConditionalOrder o{};
            safe_copy(o.instrument_id, "hc2510", sizeof(o.instrument_id));
            o.type = ConditionType::TakeProfit;
            o.direction = Direction::Buy;
            o.trigger_price = 3700.0 + i * 0.01;
            o.volume = 1;
            mgr.add(o);
            ++add_count;
        }
        done.store(true);
    });

    int total_triggered = 0;
    while (!done.load()) {
        TickData tick{};
        safe_copy(tick.instrument_id, "rb2510", sizeof(tick.instrument_id));
        tick.last_price = 3400.0;
        auto result = mgr.check_tick(tick, [](const OrderRequest&, std::string&) {
            return ConditionalTriggerResult::Sent;
        });
        total_triggered += (int)result.triggered_group_ids.size();
    }

    adder.join();

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    printf("    [info] CondOrder concurrent: %d initial + %d added, %d triggered, %.2f ms\n",
           NUM_ORDERS, add_count.load(), total_triggered, ms);
}

// QPC-precision check_tick latency test with 1000 conditional orders (QPC 精度 check_tick 延迟测试, 1000 个条件单)
void test_stress_tick_latency() {
    ConditionalOrderManager mgr;

    for (int i = 0; i < 1000; ++i) {
        ConditionalOrder o{};
        safe_copy(o.instrument_id, "rb2510", sizeof(o.instrument_id));
        o.type = ConditionType::StopLoss;
        o.direction = Direction::Sell;
        o.trigger_price = 3500.0 - i * 0.5;
        o.volume = 1;
        mgr.add(o);
    }

    constexpr int N = 10000;
    QPCLatencyStats stats(N);

    for (int i = 0; i < N; ++i) {
        TickData tick{};
        safe_copy(tick.instrument_id, "rb2510", sizeof(tick.instrument_id));
        tick.last_price = 3500.0 + (i % 100) * 0.5;

        QPCTimer timer;
        timer.start();
        mgr.check_tick(tick, [](const OrderRequest&, std::string&) {
            return ConditionalTriggerResult::Sent;
        });
        timer.stop();
        stats.record_elapsed(timer);
    }

    stats.print("check_tick");

    auto p = stats.compute();
    TEST_ASSERT(p.p99_us < 100.0);  // p99 < 100us
}

// QPC-precision test: OrderManager hot path on_order_return + on_trade_return (QPC 精度: OrderManager 热路径)
void test_stress_order_hotpath() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    // Create 100 orders
    std::vector<OrderInfo> orders;
    for (int i = 0; i < 100; ++i) {
        OrderRequest req{};
        safe_copy(req.instrument_id, "rb2510", sizeof(req.instrument_id));
        req.direction = Direction::Buy;
        req.offset = Offset::Open;
        req.volume = 5;
        orders.push_back(mgr.create_order(req));
    }

    constexpr int N = 10000;
    QPCLatencyStats stats(N);

    // Measure on_order_return latency
    for (int i = 0; i < N; ++i) {
        OrderInfo ret = orders[i % 100];
        ret.status = (i % 3 == 0) ? OrderStatus::PartTraded : OrderStatus::Pending;
        ret.traded_volume = (i % 3 == 0) ? 2 : 0;

        QPCTimer timer;
        timer.start();
        mgr.on_order_return(ret);
        timer.stop();
        stats.record_elapsed(timer);
    }

    stats.print("on_order_return");
    auto p = stats.compute();
    TEST_ASSERT(p.p99_us < 50.0);  // p99 < 50us

    // Measure on_trade_return latency
    stats.reset();
    for (int i = 0; i < N; ++i) {
        TradeInfo trade{};
        safe_copy(trade.instrument_id, "rb2510", sizeof(trade.instrument_id));
        safe_copy(trade.order_ref, orders[i % 100].order_ref, sizeof(trade.order_ref));
        trade.volume = 1;

        QPCTimer timer;
        timer.start();
        mgr.on_trade_return(trade);
        timer.stop();
        stats.record_elapsed(timer);
    }

    stats.print("on_trade_return");
    p = stats.compute();
    TEST_ASSERT(p.p99_us < 50.0);  // p99 < 50us
}

// QPC-precision test: PositionManager hot path on_trade (QPC 精度: PositionManager 热路径)
void test_stress_position_hotpath() {
    PositionManager mgr;
    constexpr int N = 10000;
    QPCLatencyStats stats(N);

    for (int i = 0; i < N; ++i) {
        TradeInfo trade{};
        safe_copy(trade.instrument_id, "rb2510", sizeof(trade.instrument_id));
        trade.direction = (i % 2 == 0) ? Direction::Buy : Direction::Sell;
        trade.offset = (i % 5 == 0) ? Offset::Close : Offset::Open;
        trade.price = 3500.0 + (i % 100) * 0.1;
        trade.volume = 1;

        QPCTimer timer;
        timer.start();
        mgr.on_trade(trade);
        timer.stop();
        stats.record_elapsed(timer);
    }

    stats.print("on_trade");
    auto p = stats.compute();
    TEST_ASSERT(p.p99_us < 50.0);  // p99 < 50us
}

// QPC-precision test: PositionManager get_position hot path query (QPC 精度: PositionManager 查询热路径)
void test_stress_position_query() {
    PositionManager mgr;
    // Pre-populate with 20 positions
    for (int i = 0; i < 20; ++i) {
        TradeInfo trade{};
        safe_copy(trade.instrument_id, ("rb" + std::to_string(2510 + i)).c_str(), sizeof(trade.instrument_id));
        trade.direction = Direction::Buy;
        trade.offset = Offset::Open;
        trade.price = 3500.0;
        trade.volume = 10;
        mgr.on_trade(trade);
    }

    constexpr int N = 10000;
    QPCLatencyStats stats(N);

    for (int i = 0; i < N; ++i) {
        char inst[16];
        safe_copy(inst, ("rb" + std::to_string(2510 + (i % 20))).c_str(), sizeof(inst));

        QPCTimer timer;
        timer.start();
        auto pos = mgr.get_position(inst, Direction::Buy);
        timer.stop();
        stats.record_elapsed(timer);
        (void)pos;
    }

    stats.print("get_position");
    auto p = stats.compute();
    TEST_ASSERT(p.p99_us < 50.0);  // p99 < 50us
}
