// test_order_manager.cpp - OrderManager unit tests (OrderManager 单元测试)

#include "order/order_manager.h"
#include "common/types.h"
#include <stdexcept>
#include <string>

extern void TEST_SUITE(const char*);
#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond \
        + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)
#define TEST_ASSERT_EQ(a, b)                                                  \
    do { if ((a) != (b)) throw std::runtime_error(std::string("EQ: ") + #a +   \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)

using namespace hft;

static OrderRequest make_req(const char* instr, Direction dir, Offset offset, int volume) {
    OrderRequest r{};
    safe_copy(r.instrument_id, instr, sizeof(r.instrument_id));
    safe_copy(r.account_id, "TestAcct", sizeof(r.account_id));
    safe_copy(r.strategy_id, "TestStrat", sizeof(r.strategy_id));
    r.direction = dir;
    r.offset = offset;
    r.price = 3500.0;
    r.volume = volume;
    return r;
}

void test_order_create() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    auto req = make_req("rb2510", Direction::Buy, Offset::Open, 5);
    auto order = mgr.create_order(req);

    TEST_ASSERT(order.order_ref[0] != '\0');
    TEST_ASSERT_EQ(std::string(order.instrument_id), "rb2510");
    TEST_ASSERT_EQ(order.total_volume, 5);
    TEST_ASSERT_EQ(order.traded_volume, 0);
    TEST_ASSERT(order.status == OrderStatus::Pending);
    TEST_ASSERT_EQ(order.front_id, 1);
    TEST_ASSERT_EQ(order.session_id, 100);
}

void test_order_return_update() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    auto req = make_req("rb2510", Direction::Buy, Offset::Open, 5);
    auto order = mgr.create_order(req);
    std::string ref(order.order_ref);

    // Simulate CTP return: partial fill (模拟 CTP 回报: 部分成交)
    OrderInfo ret{};
    safe_copy(ret.order_ref, ref.c_str(), sizeof(ret.order_ref));
    ret.traded_volume = 3;
    ret.status = OrderStatus::PartTraded;
    mgr.on_order_return(ret);

    OrderInfo copy;
    TEST_ASSERT(mgr.get_order_copy(ref, copy));
    TEST_ASSERT_EQ(copy.traded_volume, 3);
    TEST_ASSERT(copy.status == OrderStatus::PartTraded);
}

void test_order_trade_return() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    auto req = make_req("rb2510", Direction::Buy, Offset::Open, 5);
    auto order = mgr.create_order(req);
    std::string ref(order.order_ref);

    // Simulate trade return (模拟成交回报)
    TradeInfo trade{};
    safe_copy(trade.order_ref, ref.c_str(), sizeof(trade.order_ref));
    safe_copy(trade.instrument_id, "rb2510", sizeof(trade.instrument_id));
    trade.volume = 2;
    mgr.on_trade_return(trade);

    OrderInfo copy;
    TEST_ASSERT(mgr.get_order_copy(ref, copy));
    TEST_ASSERT_EQ(copy.traded_volume, 2);
    TEST_ASSERT(copy.status == OrderStatus::PartTraded);

    // Another trade of 3 lots (再成交 3 手)
    trade.volume = 3;
    mgr.on_trade_return(trade);
    TEST_ASSERT(mgr.get_order_copy(ref, copy));
    TEST_ASSERT_EQ(copy.traded_volume, 5);
    TEST_ASSERT(copy.status == OrderStatus::AllTraded);
}

void test_order_traded_volume_monotonic() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    auto req = make_req("rb2510", Direction::Buy, Offset::Open, 10);
    auto order = mgr.create_order(req);
    std::string ref(order.order_ref);

    // CTP may report traded_volume=5 first, then 3 (out of order) — CTP 可能先报 5 再报 3 (乱序)
    OrderInfo ret1{};
    safe_copy(ret1.order_ref, ref.c_str(), sizeof(ret1.order_ref));
    ret1.traded_volume = 5;
    ret1.status = OrderStatus::PartTraded;
    mgr.on_order_return(ret1);

    OrderInfo ret2{};
    safe_copy(ret2.order_ref, ref.c_str(), sizeof(ret2.order_ref));
    ret2.traded_volume = 3; // Small than previous (比之前小)
    ret2.status = OrderStatus::PartTraded;
    mgr.on_order_return(ret2);

    OrderInfo copy;
    TEST_ASSERT(mgr.get_order_copy(ref, copy));
    TEST_ASSERT_EQ(copy.traded_volume, 5); // max(5,3) = 5
}

void test_order_active_filter() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    auto r1 = mgr.create_order(make_req("rb2510", Direction::Buy, Offset::Open, 5));
    auto r2 = mgr.create_order(make_req("hc2510", Direction::Sell, Offset::Open, 3));
    auto r3 = mgr.create_order(make_req("rb2510", Direction::Buy, Offset::Open, 2));

    // Mark r2 as Cancelled (把 r2 设为 Cancelled)
    OrderInfo ret{};
    safe_copy(ret.order_ref, r2.order_ref, sizeof(ret.order_ref));
    ret.status = OrderStatus::Cancelled;
    mgr.on_order_return(ret);

    auto active = mgr.get_active_orders();
    TEST_ASSERT_EQ(active.size(), 2u); // r1 和 r3
}

void test_order_pending_volume() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    // Two open buy orders (两个开仓买单)
    auto r1 = mgr.create_order(make_req("rb2510", Direction::Buy, Offset::Open, 5));
    auto r2 = mgr.create_order(make_req("rb2510", Direction::Buy, Offset::Open, 3));

    int pending = mgr.get_pending_open_volume("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pending, 8); // 5+3

    // Partial fill on r1 (部分成交 r1)
    TradeInfo trade{};
    safe_copy(trade.order_ref, r1.order_ref, sizeof(trade.order_ref));
    safe_copy(trade.instrument_id, "rb2510", sizeof(trade.instrument_id));
    trade.volume = 4;
    mgr.on_trade_return(trade);

    pending = mgr.get_pending_open_volume("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pending, 4); // (5-4)+3 = 4
}

// Test: CTP duplicate trade callback should not inflate traded_volume (CTP 重复成交回调不应导致 traded_volume 虚增)
void test_order_trade_dedup() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    auto req = make_req("rb2510", Direction::Buy, Offset::Open, 5);
    auto order = mgr.create_order(req);
    std::string ref(order.order_ref);

    // First trade (第一次成交)
    TradeInfo trade{};
    safe_copy(trade.order_ref, ref.c_str(), sizeof(trade.order_ref));
    safe_copy(trade.instrument_id, "rb2510", sizeof(trade.instrument_id));
    safe_copy(trade.trade_id, "T001", sizeof(trade.trade_id));
    trade.volume = 2;
    mgr.on_trade_return(trade);

    OrderInfo copy;
    TEST_ASSERT(mgr.get_order_copy(ref, copy));
    TEST_ASSERT_EQ(copy.traded_volume, 2);

    // Duplicate trade (same trade_id) - should be deduped (重复成交, 相同 trade_id, 应被去重)
    mgr.on_trade_return(trade);
    TEST_ASSERT(mgr.get_order_copy(ref, copy));
    TEST_ASSERT_EQ(copy.traded_volume, 2); // Should not become 4 (不应变成 4)

    // Different trade_id - should accumulate normally (不同 trade_id, 应正常累加)
    safe_copy(trade.trade_id, "T002", sizeof(trade.trade_id));
    trade.volume = 1;
    mgr.on_trade_return(trade);
    TEST_ASSERT(mgr.get_order_copy(ref, copy));
    TEST_ASSERT_EQ(copy.traded_volume, 3); // 2+1 = 3
}

// Test: Terminal state (AllTraded/Cancelled) must not be overwritten by non-terminal state
// 测试: 终态 (AllTraded/Cancelled) 不允许被非终态覆盖
void test_order_terminal_state_protection() {
    OrderManager mgr;
    mgr.init(1, 100, 1);

    auto req = make_req("rb2510", Direction::Buy, Offset::Open, 5);
    auto order = mgr.create_order(req);
    std::string ref(order.order_ref);

    // Set as AllTraded (设为 AllTraded)
    OrderInfo ret{};
    safe_copy(ret.order_ref, ref.c_str(), sizeof(ret.order_ref));
    ret.status = OrderStatus::AllTraded;
    ret.traded_volume = 5;
    mgr.on_order_return(ret);

    // Try to overwrite with PartTraded - should be rejected (尝试用 PartTraded 覆盖, 应被拒绝)
    ret.status = OrderStatus::PartTraded;
    ret.traded_volume = 3;
    mgr.on_order_return(ret);

    OrderInfo copy;
    TEST_ASSERT(mgr.get_order_copy(ref, copy));
    TEST_ASSERT(copy.status == OrderStatus::AllTraded); // Remain terminal state (保持终态)
    TEST_ASSERT_EQ(copy.traded_volume, 5); // Unchanged (不变)

    // Cancelled is also a terminal state (Cancelled 也是终态)
    auto req2 = make_req("hc2510", Direction::Sell, Offset::Open, 3);
    auto order2 = mgr.create_order(req2);
    std::string ref2(order2.order_ref);

    OrderInfo ret2{};
    safe_copy(ret2.order_ref, ref2.c_str(), sizeof(ret2.order_ref));
    ret2.status = OrderStatus::Cancelled;
    mgr.on_order_return(ret2);

    // Try to overwrite Cancelled with Pending - should be rejected (尝试用 Pending 覆盖 Cancelled, 应被拒绝)
    ret2.status = OrderStatus::Pending;
    mgr.on_order_return(ret2);

    TEST_ASSERT(mgr.get_order_copy(ref2, copy));
    TEST_ASSERT(copy.status == OrderStatus::Cancelled); // 保持终态

    // on_trade_return should also not overwrite Cancelled (on_trade_return 也不应覆盖 Cancelled)
    TradeInfo trade{};
    safe_copy(trade.order_ref, ref2.c_str(), sizeof(trade.order_ref));
    safe_copy(trade.instrument_id, "hc2510", sizeof(trade.instrument_id));
    trade.volume = 1;
    mgr.on_trade_return(trade);

    TEST_ASSERT(mgr.get_order_copy(ref2, copy));
    TEST_ASSERT(copy.status == OrderStatus::Cancelled); // Still terminal state (仍然保持终态)
}
