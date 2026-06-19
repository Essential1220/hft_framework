// ============================================
// test_main.cpp - HFT Framework test entry point (HFT Framework 测试入口)
// Lightweight test framework with zero external dependencies
// 零外部依赖的轻量测试框架
// ============================================

#include "common/logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// ---- Test infrastructure (测试基础设施) ----

struct TestResult {
    std::string suite;
    std::string name;
    bool passed;
    double elapsed_ms;
    std::string error;
};

static std::vector<TestResult> g_results;
static std::string g_current_suite;
static std::string g_test_filter;

#define TEST_SUITE(name) g_current_suite = name

#define TEST_ASSERT(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            throw std::runtime_error(std::string("ASSERT FAILED: ") + #cond +  \
                                     " at " + __FILE__ + ":" +                 \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_EQ(a, b)                                                  \
    do {                                                                       \
        if ((a) != (b)) {                                                      \
            throw std::runtime_error(std::string("ASSERT_EQ FAILED: ") +       \
                                     #a " != " #b " at " + __FILE__ + ":" +   \
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

#define RUN_TEST(func)                                                        \
    do {                                                                       \
        if (!g_test_filter.empty() && std::string(#func).find(g_test_filter) == std::string::npos) { \
            break;                                                             \
        }                                                                      \
        TestResult tr;                                                         \
        tr.suite = g_current_suite;                                            \
        tr.name = #func;                                                       \
        auto _start = std::chrono::steady_clock::now();                        \
        try {                                                                  \
            func();                                                            \
            tr.passed = true;                                                  \
        } catch (const std::exception& e) {                                    \
            tr.passed = false;                                                 \
            tr.error = e.what();                                               \
        } catch (...) {                                                        \
            tr.passed = false;                                                 \
            tr.error = "unknown exception";                                    \
        }                                                                      \
        auto _end = std::chrono::steady_clock::now();                          \
        tr.elapsed_ms = std::chrono::duration<double, std::milli>(             \
            _end - _start).count();                                            \
        g_results.push_back(tr);                                               \
    } while (0)

// ---- Forward declarations of all test functions (前向声明所有测试函数) ----

// SPSCQueue
void test_spsc_push_pop();
void test_spsc_full_returns_false();
void test_spsc_empty_returns_false();
void test_spsc_size();
void test_spsc_capacity();
void test_spsc_drop_count();
void test_spsc_stress_million();

// ConditionalOrderManager
void test_cond_add_cancel();
void test_cond_stop_loss_trigger();
void test_cond_take_profit_trigger();
void test_cond_trailing_stop();
void test_cond_callback_no_deadlock();
void test_cond_oco_group();
void test_cond_retry_backoff();
void test_cond_active_count();
void test_cond_cancel_all();
void test_cond_cancel_removes_index_entry();
void test_cond_oco_cancelled_blocks_companion();
void test_cond_stress();

// OrderManager
void test_order_create();
void test_order_return_update();
void test_order_trade_return();
void test_order_traded_volume_monotonic();
void test_order_active_filter();
void test_order_pending_volume();
void test_order_trade_dedup();
void test_order_terminal_state_protection();

// PositionManager
void test_pos_open_long();
void test_pos_open_short();
void test_pos_close();
void test_pos_close_today_yesterday();
void test_pos_avg_price();
void test_pos_net_position();
void test_pos_replace_snapshot();

// RiskManager
void test_risk_volume_zero();
void test_risk_volume_exceed();
void test_risk_risk_reduction_exemption();
void test_risk_net_position_projection();
void test_risk_closeable_position();
void test_risk_daily_loss();

// Cancel-rate exempt + Conditional order TTL (2026-05-28 SimNow test report fixes) — 撤单率豁免 + 条件单 TTL 修复
void test_cancel_rate_exempt_empty_list_matches_nothing();
void test_cancel_rate_exempt_suffix_glob();
void test_cancel_rate_exempt_prefix_glob();
void test_cancel_rate_exempt_contains_and_exact();
void test_cancel_rate_exempt_check_order_pass();
void test_cancel_rate_exempt_still_subject_to_order_size();
void test_cond_order_ttl_zero_never_expires();
void test_cond_order_ttl_within_ttl_keeps();
void test_cond_order_ttl_exceeded_expires();
void test_cond_order_ttl_unknown_created_does_not_expire();

// Stress tests
void test_stress_spsc_throughput();
void test_stress_cond_order_concurrent();
void test_stress_tick_latency();
void test_stress_order_hotpath();
void test_stress_position_hotpath();
void test_stress_position_query();

// Integration tests
void test_integration_order_lifecycle();
void test_integration_risk_rejection();
void test_integration_close_lifecycle();
void test_integration_multi_instrument();
void test_integration_net_position_projection();
void test_integration_cancel_updates_pending();
void test_integration_insufficient_closeable();
void test_integration_daily_loss_limit();
void test_integration_fake_gateway_records_orders();
void test_integration_fake_gateway_custom_handler();
void test_integration_full_pipeline_with_gateway();
void test_integration_fake_md_gateway();
void test_strategy_lifecycle_is_per_strategy();
void test_strategy_snapshot_includes_runtime_position_stats();
void test_engine_md_queue_overflow_is_reported();
void test_engine_production_hft_config();
void test_engine_trade_queue_overflow_halts_trading();
void test_engine_command_queue_overflow_fallback_executes_control_command();

// RMS tests
void test_rms_normal_allows_open();
void test_rms_no_open_blocks_open();
void test_rms_halted_blocks_all();
void test_rms_reduce_only_blocks_open();
void test_rms_liquidating_blocks_open();
void test_rms_risk_event_push();
void test_rms_mode_transition();
void test_rms_snapshot_includes_mode();
void test_rms_error_code_strings();
void test_rms_mode_strings();
void test_rms_reduce_only_risk_reduction_exemption();

// Option pricing tests
void test_option_bs_call_price();
void test_option_metrics_solves_call_iv_and_greeks();
void test_option_metrics_solves_put_iv_and_delta();
void test_option_metrics_rejects_missing_market_inputs();

// WAL (Write-Ahead Log)
void test_crc32_basic();
void test_crc32_incremental();
void test_wal_roundtrip();
void test_wal_crc_corruption();
void test_wal_file_not_found();
void test_wal_empty_payload();

// Watchdog shared memory
void test_watchdog_shm_create_and_read();
void test_watchdog_shm_cross_view();
void test_watchdog_shm_stale_detection();

// Dual gateway
void test_dual_md_failover();
void test_dual_trade_failover();
void test_dual_md_no_backup();

// Feature pipeline
void test_feature_sma();
void test_feature_ema();
void test_feature_rsi();
void test_feature_bid_ask_imbalance();
void test_feature_vwap();
void test_feature_multi_instrument();
void test_feature_disabled();

// FIX gateway stub
void test_fix_md_stub();
void test_fix_trade_stub();

// Network receiver
void test_udp_receiver_loopback();
void test_udp_receiver_timeout();
void test_network_receiver_factory();

// UDP multicast gateway
void test_udp_publish_receive_roundtrip();
void test_udp_header_format();

#ifdef HFT_HAS_QDP
// QDP gateway smoke tests (only compiled when HFT_ENABLE_QDP=ON)
void test_qdp_md_gateway_can_construct();
void test_qdp_trade_gateway_can_construct();
#endif

// ---- Main function (主函数) ----

int main() {
    if (const char* env = std::getenv("HFT_TEST_FILTER")) {
        g_test_filter = env;
    }

    // Initialize logger (write to tests.log) — 初始化日志 (写入 tests.log)
    hft::Logger::instance().init("tests.log");

    printf("========================================\n");
    printf("  HFT Framework Test Suite\n");
    printf("  Date: %s\n", __DATE__);
    printf("========================================\n\n");

    auto suite_start = std::chrono::steady_clock::now();

    // ---- SPSCQueue ----
    TEST_SUITE("SPSCQueue");
    RUN_TEST(test_spsc_push_pop);
    RUN_TEST(test_spsc_full_returns_false);
    RUN_TEST(test_spsc_empty_returns_false);
    RUN_TEST(test_spsc_size);
    RUN_TEST(test_spsc_capacity);
    RUN_TEST(test_spsc_drop_count);
    RUN_TEST(test_spsc_stress_million);

    // ---- ConditionalOrderManager ----
    TEST_SUITE("ConditionalOrderManager");
    RUN_TEST(test_cond_add_cancel);
    RUN_TEST(test_cond_stop_loss_trigger);
    RUN_TEST(test_cond_take_profit_trigger);
    RUN_TEST(test_cond_trailing_stop);
    RUN_TEST(test_cond_callback_no_deadlock);
    RUN_TEST(test_cond_oco_group);
    RUN_TEST(test_cond_retry_backoff);
    RUN_TEST(test_cond_active_count);
    RUN_TEST(test_cond_cancel_all);
    RUN_TEST(test_cond_cancel_removes_index_entry);
    RUN_TEST(test_cond_oco_cancelled_blocks_companion);
    RUN_TEST(test_cond_stress);

    // ---- OrderManager ----
    TEST_SUITE("OrderManager");
    RUN_TEST(test_order_create);
    RUN_TEST(test_order_return_update);
    RUN_TEST(test_order_trade_return);
    RUN_TEST(test_order_traded_volume_monotonic);
    RUN_TEST(test_order_active_filter);
    RUN_TEST(test_order_pending_volume);
    RUN_TEST(test_order_trade_dedup);
    RUN_TEST(test_order_terminal_state_protection);

    // ---- PositionManager ----
    TEST_SUITE("PositionManager");
    RUN_TEST(test_pos_open_long);
    RUN_TEST(test_pos_open_short);
    RUN_TEST(test_pos_close);
    RUN_TEST(test_pos_close_today_yesterday);
    RUN_TEST(test_pos_avg_price);
    RUN_TEST(test_pos_net_position);
    RUN_TEST(test_pos_replace_snapshot);

    // ---- RiskManager ----
    TEST_SUITE("RiskManager");
    RUN_TEST(test_risk_volume_zero);
    RUN_TEST(test_risk_volume_exceed);
    RUN_TEST(test_risk_risk_reduction_exemption);
    RUN_TEST(test_risk_net_position_projection);
    RUN_TEST(test_risk_closeable_position);
    RUN_TEST(test_risk_daily_loss);
    RUN_TEST(test_cancel_rate_exempt_empty_list_matches_nothing);
    RUN_TEST(test_cancel_rate_exempt_suffix_glob);
    RUN_TEST(test_cancel_rate_exempt_prefix_glob);
    RUN_TEST(test_cancel_rate_exempt_contains_and_exact);
    RUN_TEST(test_cancel_rate_exempt_check_order_pass);
    RUN_TEST(test_cancel_rate_exempt_still_subject_to_order_size);
    RUN_TEST(test_cond_order_ttl_zero_never_expires);
    RUN_TEST(test_cond_order_ttl_within_ttl_keeps);
    RUN_TEST(test_cond_order_ttl_exceeded_expires);
    RUN_TEST(test_cond_order_ttl_unknown_created_does_not_expire);

    // ---- Stress ----
    TEST_SUITE("Stress");
    RUN_TEST(test_stress_spsc_throughput);
    RUN_TEST(test_stress_cond_order_concurrent);
    RUN_TEST(test_stress_tick_latency);
    RUN_TEST(test_stress_order_hotpath);
    RUN_TEST(test_stress_position_hotpath);
    RUN_TEST(test_stress_position_query);

    // ---- Integration ----
    TEST_SUITE("Integration");
    RUN_TEST(test_integration_order_lifecycle);
    RUN_TEST(test_integration_risk_rejection);
    RUN_TEST(test_integration_close_lifecycle);
    RUN_TEST(test_integration_multi_instrument);
    RUN_TEST(test_integration_net_position_projection);
    RUN_TEST(test_integration_cancel_updates_pending);
    RUN_TEST(test_integration_insufficient_closeable);
    RUN_TEST(test_integration_daily_loss_limit);
    RUN_TEST(test_integration_fake_gateway_records_orders);
    RUN_TEST(test_integration_fake_gateway_custom_handler);
    RUN_TEST(test_integration_full_pipeline_with_gateway);
    RUN_TEST(test_integration_fake_md_gateway);
    RUN_TEST(test_strategy_lifecycle_is_per_strategy);
    RUN_TEST(test_strategy_snapshot_includes_runtime_position_stats);
    RUN_TEST(test_engine_md_queue_overflow_is_reported);
    RUN_TEST(test_engine_production_hft_config);
    RUN_TEST(test_engine_trade_queue_overflow_halts_trading);
    RUN_TEST(test_engine_command_queue_overflow_fallback_executes_control_command);

    // ---- RMS ----
    TEST_SUITE("RMS");
    RUN_TEST(test_rms_normal_allows_open);
    RUN_TEST(test_rms_no_open_blocks_open);
    RUN_TEST(test_rms_halted_blocks_all);
    RUN_TEST(test_rms_reduce_only_blocks_open);
    RUN_TEST(test_rms_liquidating_blocks_open);
    RUN_TEST(test_rms_risk_event_push);
    RUN_TEST(test_rms_mode_transition);
    RUN_TEST(test_rms_snapshot_includes_mode);
    RUN_TEST(test_rms_error_code_strings);
    RUN_TEST(test_rms_mode_strings);
    RUN_TEST(test_rms_reduce_only_risk_reduction_exemption);

    // ---- Option pricing ----
    TEST_SUITE("OptionPricing");
    RUN_TEST(test_option_bs_call_price);
    RUN_TEST(test_option_metrics_solves_call_iv_and_greeks);
    RUN_TEST(test_option_metrics_solves_put_iv_and_delta);
    RUN_TEST(test_option_metrics_rejects_missing_market_inputs);

    // ---- WAL ----
    TEST_SUITE("WAL");
    RUN_TEST(test_crc32_basic);
    RUN_TEST(test_crc32_incremental);
    RUN_TEST(test_wal_roundtrip);
    RUN_TEST(test_wal_crc_corruption);
    RUN_TEST(test_wal_file_not_found);
    RUN_TEST(test_wal_empty_payload);

    // ---- Watchdog ----
    TEST_SUITE("Watchdog");
    RUN_TEST(test_watchdog_shm_create_and_read);
    RUN_TEST(test_watchdog_shm_cross_view);
    RUN_TEST(test_watchdog_shm_stale_detection);

    // ---- Dual Gateway ----
    TEST_SUITE("DualGateway");
    RUN_TEST(test_dual_md_failover);
    RUN_TEST(test_dual_trade_failover);
    RUN_TEST(test_dual_md_no_backup);

    // ---- Feature Pipeline ----
    TEST_SUITE("FeaturePipeline");
    RUN_TEST(test_feature_sma);
    RUN_TEST(test_feature_ema);
    RUN_TEST(test_feature_rsi);
    RUN_TEST(test_feature_bid_ask_imbalance);
    RUN_TEST(test_feature_vwap);
    RUN_TEST(test_feature_multi_instrument);
    RUN_TEST(test_feature_disabled);

    // ---- FIX Gateway ----
    TEST_SUITE("FixGateway");
    RUN_TEST(test_fix_md_stub);
    RUN_TEST(test_fix_trade_stub);

    // ---- Network Receiver ----
    TEST_SUITE("NetworkReceiver");
    RUN_TEST(test_udp_receiver_loopback);
    RUN_TEST(test_udp_receiver_timeout);
    RUN_TEST(test_network_receiver_factory);

    // ---- UDP Gateway ----
    TEST_SUITE("UdpGateway");
    RUN_TEST(test_udp_publish_receive_roundtrip);
    RUN_TEST(test_udp_header_format);

#ifdef HFT_HAS_QDP
    // ---- QDP gateway smoke ----
    TEST_SUITE("QdpGateway");
    RUN_TEST(test_qdp_md_gateway_can_construct);
    RUN_TEST(test_qdp_trade_gateway_can_construct);
#endif

    auto suite_end = std::chrono::steady_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(
        suite_end - suite_start).count();

    // ---- Output report (输出报告) ----
    printf("\n========================================\n");
    printf("  Test Results\n");
    printf("========================================\n\n");

    int passed = 0, failed = 0;
    std::string last_suite;

    for (const auto& r : g_results) {
        if (r.suite != last_suite) {
            if (!last_suite.empty()) printf("\n");
            printf("[%s]\n", r.suite.c_str());
            last_suite = r.suite;
        }
        if (r.passed) {
            printf("  PASSED  %-50s %7.2f ms\n", r.name.c_str(), r.elapsed_ms);
            ++passed;
        } else {
            printf("  FAILED  %-50s %7.2f ms\n", r.name.c_str(), r.elapsed_ms);
            printf("          %s\n", r.error.c_str());
            ++failed;
        }
    }

    printf("\n========================================\n");
    printf("  Total: %d | Passed: %d | Failed: %d\n", passed + failed, passed, failed);
    printf("  Elapsed: %.2f ms\n", total_ms);
    printf("  Result: %s\n", failed == 0 ? "ALL PASSED" : "SOME FAILED");
    printf("========================================\n");

    hft::Logger::instance().shutdown();

    return failed == 0 ? 0 : 1;
}
