// test_position_manager.cpp - PositionManager unit tests (PositionManager 单元测试)

#include "position/position_manager.h"
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
#define TEST_ASSERT_NEAR(a, b, eps)                                           \
    do { double _d = (double)(a) - (double)(b); if (_d<0) _d=-_d;             \
        if (_d > (eps)) throw std::runtime_error(std::string("NEAR: ") +       \
        #a " != " #b " diff=" + std::to_string(_d)); } while(0)

using namespace hft;

static TradeInfo make_trade(const char* instr, Direction dir, Offset offset, int vol, double price = 3500.0) {
    TradeInfo t{};
    safe_copy(t.instrument_id, instr, sizeof(t.instrument_id));
    safe_copy(t.account_id, "TestAcct", sizeof(t.account_id));
    t.direction = dir;
    t.offset = offset;
    t.volume = vol;
    t.price = price;
    return t;
}

void test_pos_open_long() {
    PositionManager pm;
    pm.on_trade(make_trade("rb2510", Direction::Buy, Offset::Open, 5));

    auto pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.total, 5);
    TEST_ASSERT_EQ(pos.today, 5);
    TEST_ASSERT_EQ(pos.yesterday, 0);
}

void test_pos_open_short() {
    PositionManager pm;
    pm.on_trade(make_trade("rb2510", Direction::Sell, Offset::Open, 3));

    auto pos = pm.get_position("rb2510", Direction::Sell);
    TEST_ASSERT_EQ(pos.total, 3);
    TEST_ASSERT_EQ(pos.today, 3);
}

void test_pos_close() {
    PositionManager pm;
    // Open long 5 lots (开多头 5 手)
    pm.on_trade(make_trade("rb2510", Direction::Buy, Offset::Open, 5));
    // Sell close 3 lots = close long via Sell + Close (卖平 3 手, 平多头 = Sell + Close)
    pm.on_trade(make_trade("rb2510", Direction::Sell, Offset::Close, 3));

    auto pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.total, 2);
}

void test_pos_close_today_yesterday() {
    PositionManager pm;
    // Open 10 lots, all today position (开仓 10 手, 全部为今仓)
    pm.on_trade(make_trade("rb2510", Direction::Buy, Offset::Open, 10));

    // Simulate yesterday position by direct snapshot (模拟有昨仓: 直接设置快照)
    PositionInfo snapshot{};
    safe_copy(snapshot.instrument_id, "rb2510", sizeof(snapshot.instrument_id));
    snapshot.direction = Direction::Buy;
    snapshot.total = 10;
    snapshot.today = 6;
    snapshot.yesterday = 4;
    snapshot.avg_price = 3500.0;
    pm.update_position("rb2510", Direction::Buy, snapshot);

    // CloseToday: close 2 today lots (平 2 手今仓)
    pm.on_trade(make_trade("rb2510", Direction::Sell, Offset::CloseToday, 2));
    auto pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.today, 4); // 6 - 2 = 4
    TEST_ASSERT_EQ(pos.total, 8); // 10 - 2 = 8

    // Close: close 3 lots (yesterday first, then today) — 平 3 手, 先扣昨仓再扣今仓
    pm.on_trade(make_trade("rb2510", Direction::Sell, Offset::Close, 3));
    pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.yesterday, 1); // 4-3 = 1
    TEST_ASSERT_EQ(pos.today, 4);
    TEST_ASSERT_EQ(pos.total, 5); // 8-3 = 5
}

void test_pos_avg_price() {
    PositionManager pm;
    // Open 5 lots at 3500 (以 3500 开 5 手)
    pm.on_trade(make_trade("rb2510", Direction::Buy, Offset::Open, 5, 3500.0));
    auto pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_NEAR(pos.avg_price, 3500.0, 0.01);

    // Open another 5 at 3600 => avg = (3500*5 + 3600*5)/10 = 3550 (以 3600 再开 5 手, 均价=3550)
    pm.on_trade(make_trade("rb2510", Direction::Buy, Offset::Open, 5, 3600.0));
    pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_NEAR(pos.avg_price, 3550.0, 0.01);
    TEST_ASSERT_EQ(pos.total, 10);
}

void test_pos_net_position() {
    PositionManager pm;
    pm.on_trade(make_trade("rb2510", Direction::Buy, Offset::Open, 5));
    pm.on_trade(make_trade("rb2510", Direction::Sell, Offset::Open, 3));

    int net = pm.get_net_position("rb2510");
    TEST_ASSERT_EQ(net, 2); // 5 long - 3 short = 2 (5 多 - 3 空 = 2)
}

void test_pos_replace_snapshot() {
    PositionManager pm;
    pm.on_trade(make_trade("rb2510", Direction::Buy, Offset::Open, 5));

    // 快照恢复 (restore from snapshot)
    std::vector<PositionInfo> snapshot;
    PositionInfo p1{};
    safe_copy(p1.instrument_id, "hc2510", sizeof(p1.instrument_id));
    p1.direction = Direction::Buy;
    p1.total = 8;
    p1.avg_price = 3700.0;
    snapshot.push_back(p1);

    pm.replace_positions(snapshot);
    auto all = pm.get_all_positions();
    TEST_ASSERT_EQ(all.size(), 1u);
    TEST_ASSERT_EQ(std::string(all[0].instrument_id), "hc2510");
    TEST_ASSERT_EQ(all[0].total, 8);
}
