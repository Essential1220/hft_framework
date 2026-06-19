// test_integration.cpp - Integration test: core manager pipeline (集成测试: 核心管理器管道联调)
// Covers: tick -> risk check -> order creation -> order return -> trade return -> position update
// 覆盖: tick -> 风控 -> 下单 -> 订单回报 -> 成交回报 -> 持仓更新 完整链路

#include "order/order_manager.h"
#include "position/position_manager.h"
#include "risk/risk_manager.h"
#include "common/types.h"
#include "common/config.h"
#include "fakes/fake_trade_gateway.h"
#include "fakes/fake_md_gateway.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

extern void TEST_SUITE(const char*);
#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond \
        + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)
#define TEST_ASSERT_EQ(a, b)                                                  \
    do { if ((a) != (b)) throw std::runtime_error(std::string("EQ: ") + #a +   \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)
#define TEST_ASSERT_NEAR(a, b, eps)                                           \
    do { double _d = (double)(a) - (double)(b); if (_d < 0) _d = -_d;        \
        if (_d > (eps)) throw std::runtime_error(std::string("NEAR: ") + #a + \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)

using namespace hft;

// ---- Helper: construct OrderRequest (辅助: 构造 OrderRequest) ----
static OrderRequest make_order(const char* instr, Direction dir, Offset offset,
                                double price, int volume, const char* account = "TestAcct",
                                const char* strategy = "TestStrat") {
    OrderRequest req{};
    safe_copy(req.instrument_id, instr, sizeof(req.instrument_id));
    safe_copy(req.account_id, account, sizeof(req.account_id));
    safe_copy(req.strategy_id, strategy, sizeof(req.strategy_id));
    safe_copy(req.exchange_id, get_exchange_id(instr), sizeof(req.exchange_id));
    req.direction = dir;
    req.offset = offset;
    req.price = price;
    req.volume = volume;
    return req;
}

// ---- Helper: construct OrderInfo (simulated broker return) (辅助: 构造 OrderInfo, 模拟柜台回报) ----
static OrderInfo make_order_return(const OrderRequest& req, const std::string& order_ref,
                                    OrderStatus status, int traded_vol = 0) {
    OrderInfo info{};
    safe_copy(info.instrument_id, req.instrument_id, sizeof(info.instrument_id));
    safe_copy(info.exchange_id, req.exchange_id, sizeof(info.exchange_id));
    safe_copy(info.account_id, req.account_id, sizeof(info.account_id));
    safe_copy(info.order_ref, order_ref.c_str(), sizeof(info.order_ref));
    safe_copy(info.strategy_id, req.strategy_id, sizeof(info.strategy_id));
    info.direction = req.direction;
    info.offset = req.offset;
    info.price = req.price;
    info.total_volume = req.volume;
    info.traded_volume = traded_vol;
    info.status = status;
    info.front_id = 1;
    info.session_id = 100;
    return info;
}

// ---- Helper: construct TradeInfo (simulated broker trade) (辅助: 构造 TradeInfo, 模拟柜台成交) ----
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

// ============================================================
// Test: Full order lifecycle (open -> trade -> position update)
// 测试: 完整下单链路 (开仓 -> 成交 -> 持仓更新)
// ============================================================
void test_integration_order_lifecycle() {
    // Initialize managers (初始化管理器)
    OrderManager om;
    om.init(1, 100, 1);

    PositionManager pm;
    RiskManager rm;

    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "10");
    cfg.set_string("Risk", "MaxNetPosition", "20");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Risk", "MaxDailyLoss", "0");
    cfg.set_string("Trading", "TradingSessions", "");
    rm.init(cfg, &pm, &om);

    // Step 1: Risk check passes (风控检查通过)
    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 2);
    std::string reject_reason;
    TEST_ASSERT(rm.check_order(req, reject_reason));
    TEST_ASSERT_EQ(reject_reason, "");

    // Step 2: Create order (创建订单)
    OrderInfo order = om.create_order(req);
    TEST_ASSERT_EQ(order.total_volume, 2);
    TEST_ASSERT_EQ(order.traded_volume, 0);
    TEST_ASSERT(order.status == OrderStatus::Pending);
    std::string order_ref(order.order_ref);
    TEST_ASSERT(!order_ref.empty());

    // Step 3: Simulate broker return - partial fill (模拟柜台回报 - 部分成交)
    TradeInfo trade1 = make_trade_return(req, order_ref, "T001", 3500.0, 1);
    om.on_trade_return(trade1);
    pm.on_trade(trade1);

    // Verify position: 1 lot long (验证持仓: 1手多头)
    PositionInfo pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.total, 1);
    TEST_ASSERT_EQ(pos.today, 1);

    // Step 4: Simulate broker return - full fill (模拟柜台回报 - 全部成交)
    TradeInfo trade2 = make_trade_return(req, order_ref, "T002", 3501.0, 1);
    om.on_trade_return(trade2);
    pm.on_trade(trade2);

    // Verify position: 2 lots long, weighted avg price (验证持仓: 2手多头, 均价加权)
    pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.total, 2);
    TEST_ASSERT_EQ(pos.today, 2);
    TEST_ASSERT_NEAR(pos.avg_price, 3500.5, 0.01);

    // Step 5: Simulate broker return - order status updated to AllTraded (订单状态更新为全部成交)
    OrderInfo order_return = make_order_return(req, order_ref, OrderStatus::AllTraded, 2);
    om.on_order_return(order_return);

    // Verify active orders empty (all filled) — 验证活跃订单为空 (已全部成交)
    auto active = om.get_active_orders();
    TEST_ASSERT_EQ(active.size(), 0u);
}

// ============================================================
// Test: Risk check rejects oversized order (风险拒绝下单)
// ============================================================
void test_integration_risk_rejection() {
    OrderManager om;
    om.init(1, 100, 1);

    PositionManager pm;
    RiskManager rm;

    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "5");
    cfg.set_string("Risk", "MaxNetPosition", "10");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Trading", "TradingSessions", "");
    rm.init(cfg, &pm, &om);

    // Single order exceeds limit (单笔超限)
    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 10);
    std::string reject_reason;
    bool passed = rm.check_order(req, reject_reason);
    TEST_ASSERT(!passed);
    TEST_ASSERT(reject_reason.find("max_order_size") != std::string::npos);

    // Verify no order was created (验证没有创建订单)
    auto active = om.get_active_orders();
    TEST_ASSERT_EQ(active.size(), 0u);
}

// ============================================================
// Test: Close lifecycle (open -> close -> position cleared)
// 测试: 平仓链路 (开仓 -> 平仓 -> 持仓清零)
// ============================================================
void test_integration_close_lifecycle() {
    OrderManager om;
    om.init(1, 100, 1);

    PositionManager pm;
    RiskManager rm;

    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "10");
    cfg.set_string("Risk", "MaxNetPosition", "20");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Trading", "TradingSessions", "");
    rm.init(cfg, &pm, &om);

    // Open 3 lots long (开仓 3 手多头)
    auto open_req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 3);
    std::string reject_reason;
    TEST_ASSERT(rm.check_order(open_req, reject_reason));
    OrderInfo open_order = om.create_order(open_req);

    TradeInfo open_trade = make_trade_return(open_req, open_order.order_ref, "T001", 3500.0, 3);
    om.on_trade_return(open_trade);
    pm.on_trade(open_trade);

    PositionInfo pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.total, 3);

    // Close 2 lots via Sell Close (平仓 2 手, Sell Close)
    auto close_req = make_order("rb2510", Direction::Sell, Offset::Close, 3510.0, 2);
    TEST_ASSERT(rm.check_order(close_req, reject_reason));
    OrderInfo close_order = om.create_order(close_req);

    TradeInfo close_trade = make_trade_return(close_req, close_order.order_ref, "T002", 3510.0, 2);
    om.on_trade_return(close_trade);
    pm.on_trade(close_trade);

    // Verify position: 1 lot long (验证持仓: 1手多头)
    pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.total, 1);
    TEST_ASSERT_EQ(pos.today, 1);
}

// ============================================================
// Test: Multi-instrument position isolation (多合约持仓隔离)
// ============================================================
void test_integration_multi_instrument() {
    OrderManager om;
    om.init(1, 100, 1);

    PositionManager pm;

    // Open rb2510 long 2 lots (开仓 rb2510 多头 2 手)
    auto req1 = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 2);
    OrderInfo o1 = om.create_order(req1);
    TradeInfo t1 = make_trade_return(req1, o1.order_ref, "T001", 3500.0, 2);
    pm.on_trade(t1);

    // Open hc2510 short 1 lot (开仓 hc2510 空头 1 手)
    auto req2 = make_order("hc2510", Direction::Sell, Offset::Open, 3800.0, 1);
    OrderInfo o2 = om.create_order(req2);
    TradeInfo t2 = make_trade_return(req2, o2.order_ref, "T002", 3800.0, 1);
    pm.on_trade(t2);

    // Verify position isolation (验证持仓隔离)
    PositionInfo pos_rb = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos_rb.total, 2);

    PositionInfo pos_hc = pm.get_position("hc2510", Direction::Sell);
    TEST_ASSERT_EQ(pos_hc.total, 1);

    // Net positions (净持仓)
    TEST_ASSERT_EQ(pm.get_net_position("rb2510"), 2);
    TEST_ASSERT_EQ(pm.get_net_position("hc2510"), -1);
}

// ============================================================
// Test: Risk check - net position projection (风控 - 净持仓投影检查)
// ============================================================
void test_integration_net_position_projection() {
    OrderManager om;
    om.init(1, 100, 1);

    PositionManager pm;
    RiskManager rm;

    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "10");
    cfg.set_string("Risk", "MaxNetPosition", "3");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Trading", "TradingSessions", "");
    rm.init(cfg, &pm, &om);

    // Open 2 lots (开仓 2 手)
    auto req1 = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 2);
    std::string reject_reason;
    TEST_ASSERT(rm.check_order(req1, reject_reason));
    OrderInfo o1 = om.create_order(req1);
    TradeInfo t1 = make_trade_return(req1, o1.order_ref, "T001", 3500.0, 2);
    om.on_trade_return(t1);
    pm.on_trade(t1);

    // Open another 2 => projection 4 > max 3 => rejected (再开 2 手, 投影 4 > max 3, 拒绝)
    auto req2 = make_order("rb2510", Direction::Buy, Offset::Open, 3510.0, 2);
    TEST_ASSERT(!rm.check_order(req2, reject_reason));
    TEST_ASSERT(reject_reason.find("net position") != std::string::npos);
}

// ============================================================
// Test: Cancel updates pending open volume (订单取消后挂单量更新)
// ============================================================
void test_integration_cancel_updates_pending() {
    OrderManager om;
    om.init(1, 100, 1);

    PositionManager pm;
    RiskManager rm;

    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "10");
    cfg.set_string("Risk", "MaxNetPosition", "3");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Trading", "TradingSessions", "");
    rm.init(cfg, &pm, &om);

    // Create an open order without fill (创建一个开仓单, 不成交)
    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 2);
    std::string reject_reason;
    TEST_ASSERT(rm.check_order(req, reject_reason));
    om.create_order(req);

    // Verify pending volume (验证挂单量)
    TEST_ASSERT_EQ(om.get_pending_open_volume("rb2510", Direction::Buy), 2);

    // Another 2 => projection 0+2(existing)+2(new)=4 > 3 => rejected
    // 再开 2 手, 投影 0 + 2(已有挂单) + 2(新单) = 4 > 3, 拒绝
    auto req2 = make_order("rb2510", Direction::Buy, Offset::Open, 3510.0, 2);
    TEST_ASSERT(!rm.check_order(req2, reject_reason));
}

// ============================================================
// Test: Insufficient closeable volume rejected (平仓可平量不足拒绝)
// ============================================================
void test_integration_insufficient_closeable() {
    OrderManager om;
    om.init(1, 100, 1);

    PositionManager pm;
    RiskManager rm;

    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "10");
    cfg.set_string("Risk", "MaxNetPosition", "20");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Trading", "TradingSessions", "");
    rm.init(cfg, &pm, &om);

    // Open 1 lot (开仓 1 手)
    auto open_req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reject_reason;
    TEST_ASSERT(rm.check_order(open_req, reject_reason));
    OrderInfo o = om.create_order(open_req);
    TradeInfo t = make_trade_return(open_req, o.order_ref, "T001", 3500.0, 1);
    om.on_trade_return(t);
    pm.on_trade(t);

    // Try to close 3 lots => insufficient closeable volume (尝试平 3 手, 可平量不足)
    auto close_req = make_order("rb2510", Direction::Sell, Offset::Close, 3510.0, 3);
    TEST_ASSERT(!rm.check_order(close_req, reject_reason));
    TEST_ASSERT(reject_reason.find("insufficient") != std::string::npos);
}

// ============================================================
// Test: RiskManager daily loss limit (RiskManager 日亏损限制)
// ============================================================
void test_integration_daily_loss_limit() {
    OrderManager om;
    om.init(1, 100, 1);

    PositionManager pm;
    RiskManager rm;

    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "10");
    cfg.set_string("Risk", "MaxNetPosition", "20");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Risk", "MaxDailyLoss", "1000");
    cfg.set_string("Trading", "TradingSessions", "");
    rm.init(cfg, &pm, &om);

    // Inject initial capital (注入初始资金)
    AccountInfo acct{};
    safe_copy(acct.account_id, "TestAcct", sizeof(acct.account_id));
    acct.balance = 100000;
    rm.update_account(acct);

    // Open 1 lot before loss (先开仓 1 手, 在亏损前)
    auto open_req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    std::string reject_reason;
    TEST_ASSERT(rm.check_order(open_req, reject_reason));
    OrderInfo o = om.create_order(open_req);
    TradeInfo t = make_trade_return(open_req, o.order_ref, "T001", 3500.0, 1);
    om.on_trade_return(t);
    pm.on_trade(t);

    // Simulate loss of 2000 (exceeds 1000 limit) — 模拟亏损 2000 (超过 1000 限制)
    acct.balance = 98000;
    rm.update_account(acct);

    // Opening order should be rejected by daily loss limit (开仓应被日亏损限制拒绝)
    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3510.0, 1);
    TEST_ASSERT(!rm.check_order(req, reject_reason));
    TEST_ASSERT(reject_reason.find("daily loss") != std::string::npos);

    // Risk-reduction-exempt close should pass (1 lot closeable) — 风控豁免平仓应通过 (有 1 手可平)
    auto close_req = make_order("rb2510", Direction::Sell, Offset::Close, 3500.0, 1);
    TEST_ASSERT(rm.check_order(close_req, reject_reason, true));
}

// ============================================================
// Test: FakeTradeGateway records sent orders (FakeTradeGateway 记录发送的订单)
// ============================================================
void test_integration_fake_gateway_records_orders() {
    FakeTradeGateway gw;
    gw.init(Config(), "CTP", nullptr, "TestAcct");
    TEST_ASSERT(gw.is_logged_in());

    // Send order (发送订单)
    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 2);
    int ret = gw.send_order(req, "000001");
    TEST_ASSERT_EQ(ret, 0);

    auto sent = gw.get_sent_orders();
    TEST_ASSERT_EQ(sent.size(), 1u);
    TEST_ASSERT_EQ(std::string(sent[0].req.instrument_id), "rb2510");
    TEST_ASSERT_EQ(sent[0].order_ref, "000001");
    TEST_ASSERT_EQ(gw.get_send_count(), 1);

    // Cancel order (撤单)
    ret = gw.cancel_order("rb2510", "SHFE", "000001", 1, 100);
    TEST_ASSERT_EQ(ret, 0);
    TEST_ASSERT_EQ(gw.get_cancel_count(), 1);
    TEST_ASSERT_EQ(gw.get_cancelled_refs()[0], "000001");
}

// ============================================================
// Test: FakeTradeGateway custom send handler (FakeTradeGateway 自定义发送处理)
// ============================================================
void test_integration_fake_gateway_custom_handler() {
    FakeTradeGateway gw;
    gw.init(Config(), "CTP", nullptr, "TestAcct");

    // Set custom handler: reject all orders (设置自定义处理: 拒绝所有订单)
    gw.set_send_handler([](const OrderRequest&, const std::string&) -> int {
        return -1; // Simulate gateway rejection (模拟网关拒绝)
    });

    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 1);
    int ret = gw.send_order(req, "000001");
    TEST_ASSERT_EQ(ret, -1);
    TEST_ASSERT_EQ(gw.get_send_count(), 1);
}

// ============================================================
// Test: Full pipeline - risk -> create -> gateway send -> return -> position
// (Simulates engine send_order core path)
// 测试: 完整管道 - 风控 -> 创建 -> 网关发送 -> 回报 -> 持仓
// (模拟引擎 send_order 核心路径)
// ============================================================
void test_integration_full_pipeline_with_gateway() {
    // Initialize all components (初始化所有组件)
    OrderManager om;
    om.init(1, 100, 1);
    PositionManager pm;
    RiskManager rm;
    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "10");
    cfg.set_string("Risk", "MaxNetPosition", "20");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "100");
    cfg.set_string("Trading", "TradingSessions", "");
    rm.init(cfg, &pm, &om);

    FakeTradeGateway gw;
    gw.init(Config(), "CTP", nullptr, "TestAcct");

    // Simulate strategy emitting open signal (模拟策略发出开仓信号)
    auto req = make_order("rb2510", Direction::Buy, Offset::Open, 3500.0, 3, "TestAcct", "MomentumStrat");

    // Step 1: Risk check (风控检查)
    std::string reject_reason;
    TEST_ASSERT(rm.check_order(req, reject_reason));

    // Step 2: Create order locally (本地创建订单)
    OrderInfo order = om.create_order(req);
    std::string order_ref(order.order_ref);
    TEST_ASSERT(!order_ref.empty());

    // Step 3: Send to gateway (发送到网关)
    int ret = gw.send_order(req, order_ref);
    TEST_ASSERT_EQ(ret, 0);
    TEST_ASSERT_EQ(gw.get_send_count(), 1);
    TEST_ASSERT_EQ(gw.get_sent_orders()[0].order_ref, order_ref);

    // Step 4: Notify risk manager of sent order (通知风控已发送)
    rm.on_order_sent();

    // Step 5: Simulate broker return - all filled (模拟柜台回报 - 全部成交)
    OrderInfo order_ret = make_order_return(req, order_ref, OrderStatus::AllTraded, 3);
    om.on_order_return(order_ret);

    TradeInfo trade = make_trade_return(req, order_ref, "T001", 3500.0, 3);
    om.on_trade_return(trade);
    pm.on_trade(trade);

    // Step 6: Verify final state (验证最终状态)
    PositionInfo pos = pm.get_position("rb2510", Direction::Buy);
    TEST_ASSERT_EQ(pos.total, 3);
    TEST_ASSERT_EQ(pos.today, 3);

    auto active = om.get_active_orders();
    TEST_ASSERT_EQ(active.size(), 0u);

    TEST_ASSERT_EQ(pm.get_net_position("rb2510"), 3);
}

// ============================================================
// Test: FakeMdGateway subscription recording (FakeMdGateway 订阅记录)
// ============================================================
void test_integration_fake_md_gateway() {
    FakeMdGateway gw;
    gw.init(Config(), "CTP", nullptr);
    TEST_ASSERT(gw.is_logged_in());

    gw.subscribe({"rb2510", "hc2510", "cu2507"});
    auto subs = gw.get_subscriptions();
    TEST_ASSERT_EQ(subs.size(), 3u);
    TEST_ASSERT_EQ(subs[0], "rb2510");
    TEST_ASSERT_EQ(subs[1], "hc2510");
    TEST_ASSERT_EQ(subs[2], "cu2507");
}
