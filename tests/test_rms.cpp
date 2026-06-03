// test_rms.cpp - RMS 风控模式单元测试

#include "risk/risk_manager.h"
#include "order/order_manager.h"
#include "position/position_manager.h"
#include "common/types.h"
#include "common/config.h"

#include <string>

extern void TEST_SUITE(const char*);
#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond \
        + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)
#define TEST_ASSERT_EQ(a, b)                                                  \
    do { if ((a) != (b)) throw std::runtime_error(std::string("EQ: ") + #a +   \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)

using namespace hft;

static OrderRequest make_order(const char* instr, Direction dir, Offset offset,
                                double price, int volume) {
    OrderRequest req{};
    safe_copy(req.instrument_id, instr, sizeof(req.instrument_id));
    safe_copy(req.account_id, "TestAcct", sizeof(req.account_id));
    safe_copy(req.strategy_id, "TestStrat", sizeof(req.strategy_id));
    safe_copy(req.exchange_id, get_exchange_id(instr), sizeof(req.exchange_id));
    req.direction = dir;
    req.offset = offset;
    req.price = price;
    req.volume = volume;
    return req;
}

static TradeInfo make_trade_return(const OrderRequest& req, const std::string& order_ref,
                                    const std::string& trade_id, double price, int volume) {
    TradeInfo info{};
    safe_copy(info.instrument_id, req.instrument_id, sizeof(info.instrument_id));
    safe_copy(info.exchange_id, req.exchange_id, sizeof(info.exchange_id));
    safe_copy(info.account_id, req.account_id, sizeof(info.account_id));
    safe_copy(info.order_ref, order_ref.c_str(), sizeof(info.order_ref));
    safe_copy(info.trade_id, trade_id.c_str(), sizeof(info.trade_id));
    safe_copy(info.strategy_id, req.strategy_id, sizeof(info.strategy_id));
    info.direction = req.direction;
    info.offset = req.offset;
    info.price = price;
    info.volume = volume;
    return info;
}

static Config make_config() {
    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "10");
    cfg.set_string("Risk", "MaxNetPosition", "20");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Risk", "MaxDailyLoss", "0");
    cfg.set_string("Trading", "TradingSessions", "");
    return cfg;
}

// ============================================================
// Test: Normal mode allows open orders (Normal 模式下开仓允许)
// ============================================================
void test_rms_normal_allows_open() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::Normal);

    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reason;
    TEST_ASSERT(rm.check_order(req, reason));
}

// ============================================================
// Test: NoOpen mode blocks open, close allowed (NoOpen 模式下开仓被拒, 平仓允许)
// ============================================================
void test_rms_no_open_blocks_open() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    // First open 1 lot in Normal mode (先在 Normal 模式下开仓 1 手)
    auto open_req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reason;
    TEST_ASSERT(rm.check_order(open_req, reason));
    OrderInfo o = om.create_order(open_req);
    TradeInfo t = make_trade_return(open_req, o.order_ref, "T001", 3500.0, 1);
    om.on_trade_return(t);
    pm.on_trade(t);

    // Switch to NoOpen (切换到 NoOpen)
    rm.set_risk_mode(RiskMode::NoOpen, "test");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::NoOpen);

    // Open rejected (开仓被拒)
    auto open_req2 = make_order("rb2510", Direction::Buy, Offset::Open, 3510.0, 1);
    TEST_ASSERT(!rm.check_order(open_req2, reason));
    TEST_ASSERT(reason.find("RISK_NO_OPEN") != std::string::npos);

    // Close allowed (平仓允许)
    auto close_req = make_order("rb2510", Direction::Sell, Offset::Close, 3510.0, 1);
    TEST_ASSERT(rm.check_order(close_req, reason));
}

// ============================================================
// Test: Halted mode blocks open, close allowed (enables self-rescue)
// 测试: Halted 模式下开仓被拒, 平仓允许 (确保能平仓自救)
// ============================================================
void test_rms_halted_blocks_all() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    // First open 1 lot in Normal mode, establish closeable position (先在 Normal 模式下开仓 1 手, 建立可平仓位)
    auto open_req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reason;
    TEST_ASSERT(rm.check_order(open_req, reason));
    OrderInfo o = om.create_order(open_req);
    TradeInfo t = make_trade_return(open_req, o.order_ref, "T001", 3500.0, 1);
    om.on_trade_return(t);
    pm.on_trade(t);

    rm.set_risk_mode(RiskMode::Halted, "emergency");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::Halted);

    // Open rejected (开仓被拒)
    auto open_req2 = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    TEST_ASSERT(!rm.check_order(open_req2, reason));
    TEST_ASSERT(reason.find("RISK_HALTED") != std::string::npos);

    // Close allowed (S0-03: Halted allows close for self-rescue) — 平仓允许 (Halted 模式下允许平仓以便自救)
    auto close_req = make_order("rb2510", Direction::Sell, Offset::Close, 3510.0, 1);
    TEST_ASSERT(rm.check_order(close_req, reason));
}

// ============================================================
// Test: ReduceOnly mode blocks open, close allowed (ReduceOnly 模式下开仓被拒, 平仓允许)
// ============================================================
void test_rms_reduce_only_blocks_open() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    // First open 1 lot in Normal mode (先在 Normal 模式下开仓 1 手)
    auto open_req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reason;
    TEST_ASSERT(rm.check_order(open_req, reason));
    OrderInfo o = om.create_order(open_req);
    TradeInfo t = make_trade_return(open_req, o.order_ref, "T001", 3500.0, 1);
    om.on_trade_return(t);
    pm.on_trade(t);

    // Switch to ReduceOnly (切换到 ReduceOnly)
    rm.set_risk_mode(RiskMode::ReduceOnly, "test");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::ReduceOnly);

    // Open rejected (开仓被拒)
    auto open_req2 = make_order("rb2510", Direction::Buy, Offset::Open, 3510.0, 1);
    TEST_ASSERT(!rm.check_order(open_req2, reason));
    TEST_ASSERT(reason.find("RISK_REDUCE_ONLY") != std::string::npos);

    // Close allowed (平仓允许)
    auto close_req = make_order("rb2510", Direction::Sell, Offset::Close, 3510.0, 1);
    TEST_ASSERT(rm.check_order(close_req, reason));
}

// ============================================================
// Test: Liquidating mode blocks open, close allowed (Liquidating 模式下开仓被拒, 平仓允许)
// ============================================================
void test_rms_liquidating_blocks_open() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    // First open 1 lot in Normal mode (先在 Normal 模式下开仓 1 手)
    auto open_req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reason;
    TEST_ASSERT(rm.check_order(open_req, reason));
    OrderInfo o = om.create_order(open_req);
    TradeInfo t = make_trade_return(open_req, o.order_ref, "T001", 3500.0, 1);
    om.on_trade_return(t);
    pm.on_trade(t);

    // Switch to Liquidating (切换到 Liquidating)
    rm.set_risk_mode(RiskMode::Liquidating, "margin call");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::Liquidating);

    // Open rejected (开仓被拒)
    auto open_req2 = make_order("rb2510", Direction::Buy, Offset::Open, 3510.0, 1);
    TEST_ASSERT(!rm.check_order(open_req2, reason));
    TEST_ASSERT(reason.find("RISK_LIQUIDATING") != std::string::npos);

    // Close allowed (平仓允许)
    auto close_req = make_order("rb2510", Direction::Sell, Offset::Close, 3510.0, 1);
    TEST_ASSERT(rm.check_order(close_req, reason));
}

// ============================================================
// Test: RiskEvent push (RiskEvent 推送)
// ============================================================
void test_rms_risk_event_push() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    // Mode switch should produce RiskEvent (切换模式应产生 RiskEvent)
    rm.set_risk_mode(RiskMode::NoOpen, "test event");
    auto events = rm.drain_risk_events();
    TEST_ASSERT_EQ(events.size(), 1u);
    TEST_ASSERT(events[0].mode == RiskMode::NoOpen);
    TEST_ASSERT(events[0].timestamp_ms > 0);

    // Empty after drain (drain 后清空)
    events = rm.drain_risk_events();
    TEST_ASSERT_EQ(events.size(), 0u);

    // check_order rejection also produces RiskEvent (check_order 拒绝时也产生 RiskEvent)
    rm.set_risk_mode(RiskMode::Halted, "test");
    rm.drain_risk_events(); // Clear mode-switch event (清掉模式切换事件)

    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reason;
    rm.check_order(req, reason);
    events = rm.drain_risk_events();
    TEST_ASSERT(events.size() >= 1);
    TEST_ASSERT(events[0].error_code == RiskErrorCode::RISK_HALTED);
}

// ============================================================
// Test: RMS mode transition (RMS 模式切换)
// ============================================================
void test_rms_mode_transition() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    // Normal → Warning → NoOpen → ReduceOnly → Liquidating → Halted → Normal
    rm.set_risk_mode(RiskMode::Warning, "approaching limit");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::Warning);

    rm.set_risk_mode(RiskMode::NoOpen, "limit reached");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::NoOpen);

    rm.set_risk_mode(RiskMode::ReduceOnly, "reducing");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::ReduceOnly);

    rm.set_risk_mode(RiskMode::Liquidating, "margin call");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::Liquidating);

    rm.set_risk_mode(RiskMode::Halted, "emergency");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::Halted);

    rm.set_risk_mode(RiskMode::Normal, "recovered");
    TEST_ASSERT_EQ(rm.get_risk_mode(), RiskMode::Normal);

    // Verify all events were pushed (验证所有事件都被推送)
    auto events = rm.drain_risk_events();
    TEST_ASSERT_EQ(events.size(), 6u);
}

// ============================================================
// Test: RiskSnapshot includes RMS mode (RiskSnapshot 包含 RMS 模式)
// ============================================================
void test_rms_snapshot_includes_mode() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    auto snap = rm.get_snapshot();
    TEST_ASSERT(snap.rms_mode == RiskMode::Normal);
    TEST_ASSERT(snap.risk_level == "normal");

    rm.set_risk_mode(RiskMode::NoOpen, "test");
    snap = rm.get_snapshot();
    TEST_ASSERT(snap.rms_mode == RiskMode::NoOpen);
    TEST_ASSERT(snap.risk_level == "no_open");
}

// ============================================================
// Test: RiskErrorCode to_string (RiskErrorCode 字符串转换)
// ============================================================
void test_rms_error_code_strings() {
    TEST_ASSERT(std::string(to_string(RiskErrorCode::RISK_NO_OPEN)) == "RISK_NO_OPEN");
    TEST_ASSERT(std::string(to_string(RiskErrorCode::RISK_REDUCE_ONLY)) == "RISK_REDUCE_ONLY");
    TEST_ASSERT(std::string(to_string(RiskErrorCode::RISK_LIQUIDATING)) == "RISK_LIQUIDATING");
    TEST_ASSERT(std::string(to_string(RiskErrorCode::RISK_HALTED)) == "RISK_HALTED");
    TEST_ASSERT(std::string(to_string(RiskErrorCode::MAX_POSITION)) == "MAX_POSITION");
    TEST_ASSERT(std::string(to_string(RiskErrorCode::MAX_ORDER_SIZE)) == "MAX_ORDER_SIZE");
    TEST_ASSERT(std::string(to_string(RiskErrorCode::DAILY_LOSS_LIMIT)) == "DAILY_LOSS_LIMIT");
    TEST_ASSERT(std::string(to_string(RiskErrorCode::ORDER_RATE_LIMIT)) == "ORDER_RATE_LIMIT");
}

// ============================================================
// Test: RiskMode to_string (RiskMode 字符串转换)
// ============================================================
void test_rms_mode_strings() {
    TEST_ASSERT(std::string(to_string(RiskMode::Normal)) == "normal");
    TEST_ASSERT(std::string(to_string(RiskMode::Warning)) == "warning");
    TEST_ASSERT(std::string(to_string(RiskMode::NoOpen)) == "no_open");
    TEST_ASSERT(std::string(to_string(RiskMode::ReduceOnly)) == "reduce_only");
    TEST_ASSERT(std::string(to_string(RiskMode::Liquidating)) == "liquidating");
    TEST_ASSERT(std::string(to_string(RiskMode::Halted)) == "halted");
}

// ============================================================
// Test: ReduceOnly risk reduction exemption (ReduceOnly 风控豁免)
// ============================================================
void test_rms_reduce_only_risk_reduction_exemption() {
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    rm.init(make_config(), &pm, &om);

    rm.set_risk_mode(RiskMode::ReduceOnly, "test");

    // Risk reduction exempt open (is_risk_reduction=true) should also pass in ReduceOnly
    // 风控豁免的开仓 (is_risk_reduction=true) 在 ReduceOnly 下也应通过
    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reason;
    TEST_ASSERT(rm.check_order(req, reason, true));
}
