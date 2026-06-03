// test_risk_cancel_rate_exempt.cpp - 撤单率豁免 pattern + 条件单 TTL 单元测试
//
// 覆盖 2026-05-28 SimNow 测试报告 [HIGH] 撤单率窗口污染 与
// [MEDIUM] runtime_state 条件单 TTL 两项修复。

#include "risk/risk_manager.h"
#include "order/order_manager.h"
#include "order/conditional_order_manager.h"
#include "position/position_manager.h"
#include "common/config.h"
#include "common/types.h"

#include <stdexcept>
#include <string>

#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond \
        + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)

using namespace hft;

namespace {

Config make_cfg(const std::string& exempt_csv,
                int min_sample = 2, double max_rate = 0.5) {
    Config cfg;
    cfg.set_string("Risk", "MaxOrderSize", "100");
    cfg.set_string("Risk", "MaxNetPosition", "100");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "300");
    cfg.set_string("Risk", "CancelRateWindowMinutes", "60");
    cfg.set_string("Risk", "CancelRateMinSample", std::to_string(min_sample));
    cfg.set_string("Risk", "MaxCancelRate", std::to_string(max_rate));
    cfg.set_string("Risk", "CancelRateExemptStrategies", exempt_csv);
    return cfg;
}

OrderRequest make_req(const char* strategy_id) {
    OrderRequest r{};
    safe_copy(r.instrument_id, "rb2510", sizeof(r.instrument_id));
    safe_copy(r.strategy_id, strategy_id, sizeof(r.strategy_id));
    r.direction = Direction::Buy;
    r.offset = Offset::Open;
    r.price = 3500.0;
    r.volume = 1;
    return r;
}

// Saturate cancel rate window: send N times + cancel N times => 100% cancel rate
// 把撤单率窗口打满: 发 N 次 + 撤 N 次 -> 撤单率 100%
void poison_cancel_window(RiskManager& rm, int n) {
    for (int i = 0; i < n; ++i) {
        rm.on_order_sent();
        rm.on_cancel();
    }
}

} // namespace

// --- Pattern matching helpers: directly test is_cancel_rate_exempt behavior ---
// --- Pattern 匹配辅助函数: 直接测 is_cancel_rate_exempt 行为 ---

void test_cancel_rate_exempt_empty_list_matches_nothing() {
    PositionManager pm;
    OrderManager om; om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_cfg(""), &pm, &om);

    TEST_ASSERT(!rm.is_cancel_rate_exempt("anything"));
    TEST_ASSERT(!rm.is_cancel_rate_exempt(""));
    TEST_ASSERT(!rm.is_cancel_rate_exempt(nullptr));
}

void test_cancel_rate_exempt_suffix_glob() {
    PositionManager pm;
    OrderManager om; om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_cfg("*_test"), &pm, &om);

    TEST_ASSERT(rm.is_cancel_rate_exempt("comprehensive_test"));
    TEST_ASSERT(rm.is_cancel_rate_exempt("foo_test"));
    TEST_ASSERT(!rm.is_cancel_rate_exempt("test_strategy"));
    TEST_ASSERT(!rm.is_cancel_rate_exempt("production"));
}

void test_cancel_rate_exempt_prefix_glob() {
    PositionManager pm;
    OrderManager om; om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_cfg("test_*"), &pm, &om);

    TEST_ASSERT(rm.is_cancel_rate_exempt("test_lifecycle"));
    TEST_ASSERT(rm.is_cancel_rate_exempt("test_"));
    TEST_ASSERT(!rm.is_cancel_rate_exempt("my_test_strategy"));
}

void test_cancel_rate_exempt_contains_and_exact() {
    PositionManager pm;
    OrderManager om; om.init(1, 100, 1);
    RiskManager rm;
    rm.init(make_cfg("*demo*, fixed_id"), &pm, &om);

    TEST_ASSERT(rm.is_cancel_rate_exempt("alpha_demo_v1"));
    TEST_ASSERT(rm.is_cancel_rate_exempt("demo"));
    TEST_ASSERT(rm.is_cancel_rate_exempt("fixed_id"));
    TEST_ASSERT(!rm.is_cancel_rate_exempt("fixed_idx"));
}

// --- Real check_order path: exempt flag prevents cancel-rate rejection ---
// --- 实际 check_order 路径: 豁免位生效时不被撤单率拒 ---

void test_cancel_rate_exempt_check_order_pass() {
    PositionManager pm;
    OrderManager om; om.init(1, 100, 1);
    RiskManager rm;
    // min_sample=2, max_rate=0.5；先打满窗口让 rate=1.0 > 0.5
    rm.init(make_cfg("*_test", /*min_sample*/ 2, /*max_rate*/ 0.5), &pm, &om);
    poison_cancel_window(rm, 5);

    auto req_test = make_req("alpha_test");
    auto req_prod = make_req("alpha");
    std::string reason;

    // Matches *_test => cancel_rate_exempt=true, skip rate check => pass (命中 *_test, 跳过撤单率检查)
    TEST_ASSERT(rm.check_order(req_test, reason, /*is_risk_reduction*/ false,
                               /*cond_buy*/ 0, /*cond_sell*/ 0,
                               /*cancel_rate_exempt*/ true));

    // Normal strategy => rejected by cancel rate (普通策略 -> 撤单率拒)
    reason.clear();
    TEST_ASSERT(!rm.check_order(req_prod, reason, false, 0, 0, /*cancel_rate_exempt*/ false));
    TEST_ASSERT(reason.find("cancel rate") != std::string::npos);
}

void test_cancel_rate_exempt_still_subject_to_order_size() {
    PositionManager pm;
    OrderManager om; om.init(1, 100, 1);
    RiskManager rm;
    Config cfg = make_cfg("*_test");
    cfg.set_string("Risk", "MaxOrderSize", "5");
    rm.init(cfg, &pm, &om);

    auto req = make_req("foo_test");
    req.volume = 100;
    std::string reason;
    // Even with cancel-rate exempt, max order size still enforced (即使豁免撤单率, 单笔上限仍然生效)
    TEST_ASSERT(!rm.check_order(req, reason, false, 0, 0, /*cancel_rate_exempt*/ true));
    TEST_ASSERT(reason.find("max_order_size") != std::string::npos);
}

// --- Conditional order TTL (条件单 TTL) ---

void test_cond_order_ttl_zero_never_expires() {
    constexpr int64_t kOneYearMs = 365LL * 86400LL * 1000LL;
    TEST_ASSERT(!ConditionalOrderManager::is_expired(/*created*/ 1, /*ttl_days*/ 0,
                                                     /*now*/ kOneYearMs));
}

void test_cond_order_ttl_within_ttl_keeps() {
    constexpr int64_t kNow = 1'000'000'000'000LL;          // Arbitrary baseline (任意基准)
    constexpr int64_t kCreated = kNow - 3LL * 3600LL * 1000LL;  // 3 小时前
    TEST_ASSERT(!ConditionalOrderManager::is_expired(kCreated, /*ttl_days*/ 1, kNow));
}

void test_cond_order_ttl_exceeded_expires() {
    constexpr int64_t kNow = 1'000'000'000'000LL;
    constexpr int64_t kCreated = kNow - 2LL * 86400LL * 1000LL;  // 2 天前
    TEST_ASSERT(ConditionalOrderManager::is_expired(kCreated, /*ttl_days*/ 1, kNow));
}

void test_cond_order_ttl_unknown_created_does_not_expire() {
    // Old runtime_state files may lack created_at_ms => 0 treated as unknown, should not expire.
    // 旧 runtime_state 文件可能没有 created_at_ms -> 0 视为未知, 不应过期跳过。
    TEST_ASSERT(!ConditionalOrderManager::is_expired(/*created*/ 0, /*ttl_days*/ 1,
                                                     /*now*/ 1'000'000'000'000LL));
}
