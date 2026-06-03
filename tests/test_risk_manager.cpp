// test_risk_manager.cpp - RiskManager unit tests (RiskManager 单元测试)

#include "risk/risk_manager.h"
#include "position/position_manager.h"
#include "order/order_manager.h"
#include "common/config.h"
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

static Config make_risk_config(int max_order_size = 5, int max_net_pos = 10,
                                int max_orders_per_min = 30, double max_daily_loss = 0.0) {
    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", std::to_string(max_order_size));
    cfg.set_string("Risk", "MaxNetPosition", std::to_string(max_net_pos));
    cfg.set_string("Risk", "MaxOrdersPerMinute", std::to_string(max_orders_per_min));
    cfg.set_string("Risk", "MaxDailyLoss", std::to_string(max_daily_loss));
    cfg.set_string("Risk", "CancelRateWindowMinutes", "60");
    cfg.set_string("Risk", "MaxCancelRate", "0.5");
    return cfg;
}

static OrderRequest make_req(Direction dir, Offset offset, int volume, const char* instr = "rb2510") {
    OrderRequest r{};
    safe_copy(r.instrument_id, instr, sizeof(r.instrument_id));
    r.direction = dir;
    r.offset = offset;
    r.price = 3500.0;
    r.volume = volume;
    return r;
}

void test_risk_volume_zero() {
    PositionManager pm;
    OrderManager om;
    om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_risk_config(), &pm, &om);

    std::string reason;
    auto req = make_req(Direction::Buy, Offset::Open, 0);
    TEST_ASSERT(!rm.check_order(req, reason));
    TEST_ASSERT(reason.find("volume") != std::string::npos);
}

void test_risk_volume_exceed() {
    PositionManager pm;
    OrderManager om;
    om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_risk_config(5), &pm, &om); // max_order_size = 5

    std::string reason;
    auto req = make_req(Direction::Buy, Offset::Open, 10);
    TEST_ASSERT(!rm.check_order(req, reason));
}

void test_risk_risk_reduction_exemption() {
    PositionManager pm;
    OrderManager om;
    om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_risk_config(5), &pm, &om); // max_order_size = 5

    std::string reason;
    // Normal request exceeds limit => rejected (普通请求超量 -> 拒绝)
    auto req = make_req(Direction::Buy, Offset::Open, 10);
    TEST_ASSERT(!rm.check_order(req, reason, false));

    // Risk reduction exempt => allowed (only volume>0 and position projection checked)
    // 风控豁免 -> 允许 (只检查 volume>0 和持仓投影)
    reason.clear();
    TEST_ASSERT(rm.check_order(req, reason, true));
}

void test_risk_net_position_projection() {
    PositionManager pm;
    OrderManager om;
    om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_risk_config(100, 5), &pm, &om); // max_net_pos = 5

    std::string reason;
    // Net=0, open 6 => exceeds max_net_pos=5 (净持仓为 0, 开 6 手 -> 超过 max_net_pos=5)
    auto req = make_req(Direction::Buy, Offset::Open, 6);
    TEST_ASSERT(!rm.check_order(req, reason));

    // Open 5 => exact match (开 5 手 -> 正好)
    reason.clear();
    req = make_req(Direction::Buy, Offset::Open, 5);
    TEST_ASSERT(rm.check_order(req, reason));
}

void test_risk_closeable_position() {
    PositionManager pm;
    OrderManager om;
    om.init(1, 100, 1);

    // Set position: long 3 lots (设置持仓: 多头 3 手)
    PositionInfo pos{};
    safe_copy(pos.instrument_id, "rb2510", sizeof(pos.instrument_id));
    pos.direction = Direction::Buy;
    pos.total = 3;
    pos.today = 3;
    pm.update_position("rb2510", Direction::Buy, pos);

    RiskManager rm;
    rm.init(make_risk_config(), &pm, &om);

    std::string reason;
    // Close 5 => exceeds closeable 3 (平仓 5 手 -> 超过可平仓量 3)
    auto req = make_req(Direction::Sell, Offset::Close, 5);
    TEST_ASSERT(!rm.check_order(req, reason));

    // Close 3 => exact match (平仓 3 手 -> 正好)
    reason.clear();
    req = make_req(Direction::Sell, Offset::Close, 3);
    TEST_ASSERT(rm.check_order(req, reason));
}

void test_risk_daily_loss() {
    PositionManager pm;
    OrderManager om;
    om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_risk_config(100, 100, 30, 10000.0), &pm, &om); // max_daily_loss = 10000

    // Set initial account balance (设置账户余额)
    AccountInfo acct{};
    acct.balance = 100000.0;
    rm.update_account(acct);

    // Simulate loss exceeding 10000 (模拟亏损超过 10000)
    acct.balance = 89000.0;
    rm.update_account(acct);

    std::string reason;
    auto req = make_req(Direction::Buy, Offset::Open, 1);
    TEST_ASSERT(!rm.check_order(req, reason));

    // Risk reduction exempt can bypass daily loss check (风控豁免可以绕过日亏损检查)
    reason.clear();
    TEST_ASSERT(rm.check_order(req, reason, true));
}
