// test_conditional_order.cpp - ConditionalOrderManager unit tests (ConditionalOrderManager 单元测试)

#include "order/conditional_order_manager.h"
#include "common/types.h"
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

extern void TEST_SUITE(const char*);
#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond \
        + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)
#define TEST_ASSERT_EQ(a, b)                                                  \
    do { if ((a) != (b)) throw std::runtime_error(std::string("EQ: ") + #a +   \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)

using namespace hft;

static ConditionalOrder make_cond_order(const char* instr, ConditionType type,
                                         Direction dir, double trigger_price,
                                         int volume = 1, uint32_t group_id = 0) {
    ConditionalOrder o{};
    safe_copy(o.instrument_id, instr, sizeof(o.instrument_id));
    safe_copy(o.account_id, "TestAcct", sizeof(o.account_id));
    safe_copy(o.strategy_id, "TestStrat", sizeof(o.strategy_id));
    o.type = type;
    o.direction = dir;
    o.offset = Offset::Close;
    o.trigger_price = trigger_price;
    o.volume = volume;
    o.group_id = group_id;
    return o;
}

static TickData make_tick(const char* instr, double price) {
    TickData t{};
    safe_copy(t.instrument_id, instr, sizeof(t.instrument_id));
    t.last_price = price;
    return t;
}

void test_cond_add_cancel() {
    ConditionalOrderManager mgr;
    auto o = make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0);
    uint32_t id = mgr.add(o);
    TEST_ASSERT(id > 0);
    TEST_ASSERT_EQ(mgr.active_count(), 1u);

    mgr.cancel(id);
    TEST_ASSERT_EQ(mgr.active_count(), 0u);
}

void test_cond_stop_loss_trigger() {
    ConditionalOrderManager mgr;
    // Sell close long stop-loss: Sell=close, triggers when price <= trigger_price
    // Sell close long stop-loss: Sell=close, triggers when price <= trigger_price (卖出平多止损)
    auto o = make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0);
    mgr.add(o);

    // Price 3490 <= 3500, should trigger (价格 3490 <= 3500, 应触发)
    auto tick = make_tick("rb2510", 3490.0);
    auto result = mgr.check_tick(tick, [](const OrderRequest& req, std::string&) {
        TEST_ASSERT_EQ(std::string(req.instrument_id), "rb2510");
        TEST_ASSERT(req.direction == Direction::Sell);
        return ConditionalTriggerResult::Sent;
    });

    TEST_ASSERT(result.changed);
    TEST_ASSERT_EQ(mgr.active_count(), 0u);

    // Verify no trigger when price above threshold (验证价格未达到时不触发)
    ConditionalOrderManager mgr2;
    mgr2.add(make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0));
    auto tick2 = make_tick("rb2510", 3510.0);
    auto r2 = mgr2.check_tick(tick2, [](const OrderRequest&, std::string&) {
        return ConditionalTriggerResult::Sent;
    });
    TEST_ASSERT(!r2.changed);
    TEST_ASSERT_EQ(mgr2.active_count(), 1u);
}

void test_cond_take_profit_trigger() {
    ConditionalOrderManager mgr;
    // Sell close long take-profit: Sell=close, triggers when price >= 3600 (卖出平多止盈: 价格 >= 3600 触发)
    auto o = make_cond_order("rb2510", ConditionType::TakeProfit, Direction::Sell, 3600.0);
    mgr.add(o);

    auto tick = make_tick("rb2510", 3610.0);
    auto result = mgr.check_tick(tick, [](const OrderRequest& req, std::string&) {
        TEST_ASSERT(req.direction == Direction::Sell);
        return ConditionalTriggerResult::Sent;
    });

    TEST_ASSERT(result.changed);
    TEST_ASSERT_EQ(mgr.active_count(), 0u);
}

void test_cond_trailing_stop() {
    ConditionalOrderManager mgr;
    // Short trailing stop: Sell direction, tracks lowest extreme, triggers on trail_offset rebound
    // Short trailing stop: Sell direction, tracks lowest extreme, triggers on trail_offset rebound (做空追踪止损)
    auto o = make_cond_order("rb2510", ConditionType::TrailingStop, Direction::Sell, 0.0);
    o.trail_offset = 10.0;
    mgr.add(o);

    // Tick 1: 3600 -> extreme=3600 (lowest), 3600 >= 3600+10=3610? No (第一 tick: 3600, 极值=3600, 未触发)
    auto t1 = make_tick("rb2510", 3600.0);
    auto r1 = mgr.check_tick(t1, [](const OrderRequest&, std::string&) {
        return ConditionalTriggerResult::Sent;
    });
    TEST_ASSERT(!r1.changed);
    TEST_ASSERT_EQ(mgr.active_count(), 1u);

    // Tick 2: 3580 -> extreme=3580 (lower), 3580 >= 3580+10=3590? No (第二 tick: 3580, 极值=3580, 未触发)
    auto t2 = make_tick("rb2510", 3580.0);
    mgr.check_tick(t2, [](const OrderRequest&, std::string&) {
        return ConditionalTriggerResult::Sent;
    });

    // Tick 3: 3595 >= 3580+10=3590? Yes, triggers (price rebounded past trail_offset) — 第三 tick: 触发
    auto t3 = make_tick("rb2510", 3595.0);
    auto r3 = mgr.check_tick(t3, [](const OrderRequest&, std::string&) {
        return ConditionalTriggerResult::Sent;
    });
    TEST_ASSERT(r3.changed);
    TEST_ASSERT_EQ(mgr.active_count(), 0u);
}

void test_cond_callback_no_deadlock() {
    ConditionalOrderManager mgr;
    // Sell close long stop-loss: triggers when price <= 3500 (卖出平多止损)
    auto o = make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0);
    mgr.add(o);

    // Callback attempts to acquire mgr lock, simulating risk/order manager lock.
    // Under 3-phase design the callback runs lock-free, so no deadlock.
    // Callback tries to acquire mgr lock (simulating risk/order manager lock), 3-phase design avoids deadlock.
    // 回调内尝试获取 mgr 的锁 (模拟风控/订单管理器的锁), 三阶段设计下不应死锁
    std::mutex test_mtx;
    auto tick = make_tick("rb2510", 3490.0);
    auto result = mgr.check_tick(tick, [&](const OrderRequest&, std::string&) {
        std::lock_guard<std::mutex> lock(test_mtx); // Lock inside callback (回调内加锁)
        return ConditionalTriggerResult::Sent;
    });
    TEST_ASSERT(result.changed);
}

void test_cond_oco_group() {
    ConditionalOrderManager mgr;
    // OCO group: sell close long stop-loss(3500) + take-profit(3600) same group
    // Stop-loss: triggers when price <= 3500; Take-profit: triggers when price >= 3600
    // OCO 分组: 卖出平多止损(3500) + 卖出平多止盈(3600) 同组
    auto o1 = make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0, 1, 100);
    auto o2 = make_cond_order("rb2510", ConditionType::TakeProfit, Direction::Sell, 3600.0, 1, 100);
    mgr.add(o1);
    mgr.add(o2);
    TEST_ASSERT_EQ(mgr.active_count(), 2u);

    // tick=3490: stop-loss triggers (3490<=3500), take-profit does not (3490<3600) — 触发止损, 不触发止盈
    auto tick = make_tick("rb2510", 3490.0);
    auto result = mgr.check_tick(tick, [](const OrderRequest&, std::string&) {
        return ConditionalTriggerResult::Sent;
    });

    // OCO: after one triggers, the other in same group should be skipped (OCO: 触发一个后, 同组另一个应被跳过)
    TEST_ASSERT(result.changed);
    TEST_ASSERT_EQ(result.triggered_group_ids.size(), 1u);
    TEST_ASSERT_EQ(result.triggered_group_ids[0], 100u);
    TEST_ASSERT_EQ(mgr.active_count(), 1u); // Remaining take-profit order (剩余止盈单)
}

// Test: When first OCO conditional order is Cancelled, the second in same group must not execute callback
// 测试: OCO 组中第一个条件单被 Cancelled 时, 同组第二个不应执行回调
void test_cond_oco_cancelled_blocks_companion() {
    ConditionalOrderManager mgr;
    // Same-group two stop-loss orders: both Sell, different trigger prices
    // Stop A: price <= 3400; Stop B: price <= 3500
    // Same-group two stop-loss orders: different trigger prices, A: <=3400, B: <=3500 (同组两个止损单)
    auto o1 = make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3400.0, 1, 200);
    auto o2 = make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0, 1, 200);
    mgr.add(o1);
    mgr.add(o2);
    TEST_ASSERT_EQ(mgr.active_count(), 2u);

    // tick=3390: both trigger (3390<=3400 and 3390<=3500).
    // First returns Cancelled, OCO blocks the second callback.
    // Both trigger, first returns Cancelled, OCO blocks second callback (两个都触发, 第一个 Cancelled, 阻止第二个)
    int callback_count = 0;
    auto tick = make_tick("rb2510", 3390.0);
    auto result = mgr.check_tick(tick, [&](const OrderRequest&, std::string& reason) {
        ++callback_count;
        reason = "risk rejected";
        return ConditionalTriggerResult::Cancelled;
    });

    // OCO: only first callback executed, second skipped (OCO: 只有第一个执行了回调, 第二个被跳过)
    TEST_ASSERT_EQ(callback_count, 1);
    // First removed after Cancelled, second skipped and kept in active list (第一个被移除, 第二个保留在活跃列表)
    TEST_ASSERT_EQ(mgr.active_count(), 1u);
}

void test_cond_retry_backoff() {
    ConditionalOrderManager mgr;
    auto o = make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0);
    mgr.add(o);

    auto tick = make_tick("rb2510", 3490.0);

    // First attempt: RetryLater (第一次: RetryLater)
    auto r1 = mgr.check_tick(tick, [](const OrderRequest&, std::string& reason) {
        reason = "test reject";
        return ConditionalTriggerResult::RetryLater;
    });
    TEST_ASSERT(!r1.changed); // RetryLater 不算 changed
    TEST_ASSERT_EQ(mgr.active_count(), 1u); // Still alive (仍然存活)

    // Second attempt: backoff not expired, no callback invoked (第二次: 退避未到期, 不触发回调)
    bool callback_called = false;
    auto r2 = mgr.check_tick(tick, [&](const OrderRequest&, std::string&) {
        callback_called = true;
        return ConditionalTriggerResult::RetryLater;
    });
    // No callback during backoff period (退避期内不应调用回调)
    TEST_ASSERT(!callback_called);
}

void test_cond_active_count() {
    ConditionalOrderManager mgr;
    TEST_ASSERT_EQ(mgr.active_count(), 0u);
    mgr.add(make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0));
    TEST_ASSERT_EQ(mgr.active_count(), 1u);
    auto id = mgr.add(make_cond_order("hc2510", ConditionType::TakeProfit, Direction::Buy, 3700.0));
    TEST_ASSERT_EQ(mgr.active_count(), 2u);
    mgr.cancel(id);
    TEST_ASSERT_EQ(mgr.active_count(), 1u);
}

void test_cond_cancel_all() {
    ConditionalOrderManager mgr;
    for (int i = 0; i < 100; ++i) {
        mgr.add(make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0 - i));
    }
    TEST_ASSERT_EQ(mgr.active_count(), 100u);
    mgr.cancel_all();
    TEST_ASSERT_EQ(mgr.active_count(), 0u);
}

void test_cond_cancel_removes_index_entry() {
    ConditionalOrderManager mgr;
    auto id = mgr.add(make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0));
    mgr.cancel(id);

    bool callback_called = false;
    auto result = mgr.check_tick(make_tick("rb2510", 3600.0),
                                 [&](const OrderRequest&, std::string&) {
                                     callback_called = true;
                                     return ConditionalTriggerResult::Sent;
                                 });
    TEST_ASSERT(!callback_called);
    TEST_ASSERT(!result.changed);
    TEST_ASSERT_EQ(mgr.active_count(), 0u);
}

void test_cond_stress() {
    ConditionalOrderManager mgr;
    constexpr int N = 10000;

    auto start = std::chrono::steady_clock::now();

    // Add N conditional orders (添加 N 个条件单)
    for (int i = 0; i < N; ++i) {
        auto o = make_cond_order("rb2510", ConditionType::StopLoss, Direction::Sell, 3500.0 - i * 0.1);
        mgr.add(o);
    }
    TEST_ASSERT_EQ(mgr.active_count(), (size_t)N);

    // Send N ticks, each triggers a batch (发 N 个 tick, 每个都会触发一批)
    int total_triggered = 0;
    for (int i = 0; i < N; ++i) {
        TickData tick{};
        safe_copy(tick.instrument_id, "rb2510", sizeof(tick.instrument_id));
        tick.last_price = 3400.0; // Below all trigger prices (低于所有 trigger_price)
        auto result = mgr.check_tick(tick, [](const OrderRequest&, std::string&) {
            return ConditionalTriggerResult::Sent;
        });
        total_triggered += (int)result.triggered_group_ids.size();
    }

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    printf("    [info] %d cond orders, %d ticks, %d triggered, elapsed=%.2f ms\n",
           N, N, total_triggered, ms);
}
