// ============================================
// bench/bench_scenarios.cpp — Registered latency / throughput scenarios (已注册的延迟/吞吐场景)
// Ported from tests/test_stress.cpp, using BenchConfig.samples for sample count,
// no TEST_ASSERT (benchmark collects numbers, no hard threshold assertions).
// 移植自 tests/test_stress.cpp, 用 BenchConfig.samples 控制采样数, 不带 TEST_ASSERT (benchmark 只采集数字, 不做硬阈值断言)。
// ============================================

#include "bench/bench_runner.h"

#include "common/event.h"
#include "common/qpc_timer.h"
#include "common/spsc_queue.h"
#include "common/types.h"
#include "order/conditional_order_manager.h"
#include "order/order_manager.h"
#include "position/position_manager.h"
#include "risk/risk_manager.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace hft;
using namespace hft::bench;

// ---- 1. ConditionalOrderManager::check_tick (hottest path) — check_tick 热路径之首 ----
HFT_BENCH(cond_order_check_tick) {
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

    QPCLatencyStats stats(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) {
        TickData tick{};
        safe_copy(tick.instrument_id, "rb2510", sizeof(tick.instrument_id));
        tick.last_price = 3500.0 + (i % 100) * 0.5;

        QPCTimer t;
        t.start();
        mgr.check_tick(tick, [](const OrderRequest&, std::string&) {
            return ConditionalTriggerResult::Sent;
        });
        t.stop();
        stats.record_elapsed(t);
    }

    BenchResult r;
    r.n = cfg.samples;
    r.latency = stats.compute();
    return r;
}

// ---- 2. OrderManager::on_order_return (订单回报) ----
HFT_BENCH(order_manager_on_order_return) {
    OrderManager mgr;
    mgr.init(1, 100, 1);
    std::vector<OrderInfo> orders;
    orders.reserve(100);
    for (int i = 0; i < 100; ++i) {
        OrderRequest req{};
        safe_copy(req.instrument_id, "rb2510", sizeof(req.instrument_id));
        req.direction = Direction::Buy;
        req.offset = Offset::Open;
        req.volume = 5;
        orders.push_back(mgr.create_order(req));
    }

    QPCLatencyStats stats(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) {
        OrderInfo ret = orders[i % 100];
        ret.status = (i % 3 == 0) ? OrderStatus::PartTraded : OrderStatus::Pending;
        ret.traded_volume = (i % 3 == 0) ? 2 : 0;

        QPCTimer t;
        t.start();
        mgr.on_order_return(ret);
        t.stop();
        stats.record_elapsed(t);
    }

    BenchResult r;
    r.n = cfg.samples;
    r.latency = stats.compute();
    return r;
}

// ---- 3. OrderManager::on_trade_return (成交回报) ----
HFT_BENCH(order_manager_on_trade_return) {
    OrderManager mgr;
    mgr.init(1, 100, 1);
    std::vector<OrderInfo> orders;
    orders.reserve(100);
    for (int i = 0; i < 100; ++i) {
        OrderRequest req{};
        safe_copy(req.instrument_id, "rb2510", sizeof(req.instrument_id));
        req.direction = Direction::Buy;
        req.offset = Offset::Open;
        req.volume = 1000;  // Enough room for traded_volume growth (留够 traded_volume 增长空间)
        orders.push_back(mgr.create_order(req));
    }

    QPCLatencyStats stats(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) {
        TradeInfo trade{};
        safe_copy(trade.instrument_id, "rb2510", sizeof(trade.instrument_id));
        safe_copy(trade.order_ref, orders[i % 100].order_ref, sizeof(trade.order_ref));
        // Use unique trade_id to avoid dedup removing 99% of samples (trade_id 用唯一值, 避免去重去掉 99% 样本)
        auto tid = std::to_string(i);
        safe_copy(trade.trade_id, tid.c_str(), sizeof(trade.trade_id));
        trade.volume = 1;

        QPCTimer t;
        t.start();
        mgr.on_trade_return(trade);
        t.stop();
        stats.record_elapsed(t);
    }

    BenchResult r;
    r.n = cfg.samples;
    r.latency = stats.compute();
    return r;
}

// ---- 4. PositionManager::on_trade (持仓更新) ----
HFT_BENCH(position_on_trade) {
    PositionManager mgr;
    QPCLatencyStats stats(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) {
        TradeInfo trade{};
        safe_copy(trade.instrument_id, "rb2510", sizeof(trade.instrument_id));
        trade.direction = (i % 2 == 0) ? Direction::Buy : Direction::Sell;
        trade.offset = (i % 5 == 0) ? Offset::Close : Offset::Open;
        trade.price = 3500.0 + (i % 100) * 0.1;
        trade.volume = 1;

        QPCTimer t;
        t.start();
        mgr.on_trade(trade);
        t.stop();
        stats.record_elapsed(t);
    }

    BenchResult r;
    r.n = cfg.samples;
    r.latency = stats.compute();
    return r;
}

// ---- 5. PositionManager::get_position (hot query path) — 查询热路径 ----
HFT_BENCH(position_get_position) {
    PositionManager mgr;
    for (int i = 0; i < 20; ++i) {
        TradeInfo trade{};
        const auto inst = std::string("rb") + std::to_string(2510 + i);
        safe_copy(trade.instrument_id, inst.c_str(), sizeof(trade.instrument_id));
        trade.direction = Direction::Buy;
        trade.offset = Offset::Open;
        trade.price = 3500.0;
        trade.volume = 10;
        mgr.on_trade(trade);
    }

    QPCLatencyStats stats(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) {
        char inst[16];
        const auto s = std::string("rb") + std::to_string(2510 + (i % 20));
        safe_copy(inst, s.c_str(), sizeof(inst));

        QPCTimer t;
        t.start();
        auto pos = mgr.get_position(inst, Direction::Buy);
        t.stop();
        stats.record_elapsed(t);
        (void)pos;
    }

    BenchResult r;
    r.n = cfg.samples;
    r.latency = stats.compute();
    return r;
}

// ---- 6. SPSCQueue saturated throughput (producer + consumer) — SPSCQueue 饱和吞吐 (生产者+消费者) ----
HFT_BENCH(spsc_queue_throughput) {
    // Throughput scenario: use cfg.samples as total push/pop count (吞吐场景: 用 cfg.samples 作为总 push/pop 数量)
    SPSCQueue<size_t, 65536> q;
    const size_t N = cfg.samples * 100;  // Run larger for stability (跑大一点才稳)

    auto t0 = std::chrono::steady_clock::now();

    std::thread producer([&]() {
        for (size_t i = 0; i < N; ++i) {
            while (!q.push(i)) std::this_thread::yield();
        }
    });
    std::thread consumer([&]() {
        size_t v;
        size_t count = 0;
        while (count < N) {
            if (q.pop(v)) ++count;
            else std::this_thread::yield();
        }
    });
    producer.join();
    consumer.join();

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    double ops_per_sec = (sec > 0.0) ? (N * 2.0) / sec : 0.0;

    BenchResult r;
    r.is_throughput = true;
    r.n = N;
    r.throughput_ops_sec = ops_per_sec;
    return r;
}

// ---- 7. tick -> strategy callback end-to-end (tick -> strategy 端到端) ----
// Simulate real tick hot path: check_tick + position query + strategy on_tick.
// (No Python dispatch, C++ layer combined overhead only.)
// Simulate real tick hot path: check_tick + position query + strategy on_tick (no Python dispatch, C++ layer only)
// 模拟真实 tick 热路径: check_tick + 持仓查询 + strategy on_tick (不含 Python dispatch, 只测 C++ 层组合开销)
HFT_BENCH(tick_to_strategy_e2e) {
    ConditionalOrderManager cond_mgr;
    PositionManager pos_mgr;
    OrderManager order_mgr;
    order_mgr.init(1, 100, 1);

    // Pre-populate 100 stop-loss orders (预置 100 个止损单)
    for (int i = 0; i < 100; ++i) {
        ConditionalOrder o{};
        safe_copy(o.instrument_id, "rb2510", sizeof(o.instrument_id));
        o.type = ConditionType::StopLoss;
        o.direction = Direction::Sell;
        o.trigger_price = 3400.0 - i * 0.5;
        o.volume = 1;
        cond_mgr.add(o);
    }

    // Pre-populate positions (预置持仓)
    for (int i = 0; i < 10; ++i) {
        TradeInfo trade{};
        const auto inst = std::string("rb") + std::to_string(2510 + i);
        safe_copy(trade.instrument_id, inst.c_str(), sizeof(trade.instrument_id));
        trade.direction = Direction::Buy;
        trade.offset = Offset::Open;
        trade.price = 3500.0;
        trade.volume = 5;
        pos_mgr.on_trade(trade);
    }

    QPCLatencyStats stats(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) {
        TickData tick{};
        safe_copy(tick.instrument_id, "rb2510", sizeof(tick.instrument_id));
        tick.last_price = 3500.0 + (i % 100) * 0.5;
        tick.bid[0].price = tick.last_price - 0.2;
        tick.ask[0].price = tick.last_price + 0.2;
        tick.volume = static_cast<int>(10000 + i);

        QPCTimer t;
        t.start();

        // 1) 条件单检查 (止损/止盈/追踪)
        cond_mgr.check_tick(tick, [](const OrderRequest&, std::string&) {
            return ConditionalTriggerResult::Sent;
        });

        // 2) 持仓查询 (模拟策略读取当前仓位)
        auto pos = pos_mgr.get_position(tick.instrument_id, Direction::Buy);
        (void)pos;

        // 3) 创建一个模拟策略下单 (最简 send_order)
        OrderRequest req{};
        safe_copy(req.instrument_id, tick.instrument_id, sizeof(req.instrument_id));
        req.direction = Direction::Buy;
        req.offset = Offset::Open;
        req.price = tick.ask[0].price;
        req.volume = 1;
        order_mgr.create_order(req);

        t.stop();
        stats.record_elapsed(t);
    }

    BenchResult r;
    r.n = cfg.samples;
    r.latency = stats.compute();
    return r;
}

// ---- 8. send_order end-to-end (RiskManager + OrderManager combined) — send_order 端到端 ----
// Measure full path from OrderRequest to create_order,
// including all 8 RiskManager::check_order checks.
// 测量从 OrderRequest 到 create_order 的完整路径, 包含 RiskManager::check_order 的全部 8 项检查。
HFT_BENCH(send_order_e2e) {
    OrderManager order_mgr;
    order_mgr.init(1, 100, 1);
    PositionManager pos_mgr;
    RiskManager risk_mgr;

    // Init with default Config (default params sufficient for benchmark) — 用默认 Config 初始化 (默认参数足够 benchmark)
    Config dummy_cfg;
    risk_mgr.init(dummy_cfg, &pos_mgr, &order_mgr, "bench");

    // Pre-populate positions so risk check has real data (预置一些持仓, 让 risk check 有东西可查)
    for (int i = 0; i < 5; ++i) {
        TradeInfo trade{};
        safe_copy(trade.instrument_id, "rb2510", sizeof(trade.instrument_id));
        trade.direction = Direction::Buy;
        trade.offset = Offset::Open;
        trade.price = 3500.0;
        trade.volume = 2;
        pos_mgr.on_trade(trade);
    }

    QPCLatencyStats stats(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) {
        OrderRequest req{};
        safe_copy(req.instrument_id, "rb2510", sizeof(req.instrument_id));
        req.direction = (i % 2 == 0) ? Direction::Buy : Direction::Sell;
        req.offset = (i % 3 == 0) ? Offset::Close : Offset::Open;
        req.price = 3500.0 + (i % 100) * 0.1;
        req.volume = 1;

        QPCTimer t;
        t.start();

        std::string reject_reason;
        bool allowed = risk_mgr.check_order(req, reject_reason);
        if (allowed) {
            order_mgr.create_order(req);
            risk_mgr.on_order_sent();
        }

        t.stop();
        stats.record_elapsed(t);
        (void)allowed;
    }

    BenchResult r;
    r.n = cfg.samples;
    r.latency = stats.compute();
    return r;
}

} // namespace
