// ============================================
// test_fix_gateway.cpp - FIX gateway stub construction + return value tests
// ============================================

#include "gateway/fix_md_gateway.h"
#include "gateway/fix_trade_gateway.h"

#include <stdexcept>
#include <string>

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

void test_fix_md_stub() {
    hft::FixMdGateway gw;
    hft::Config cfg;
    gw.init(cfg, "FIX.MD", nullptr);

    TEST_ASSERT(!gw.is_logged_in());
    TEST_ASSERT(!gw.wait_for_login(1));
    TEST_ASSERT(gw.status() == hft::MdGatewayStatus::Disconnected);

    gw.subscribe({"IF2506"});
    gw.unsubscribe({"IF2506"});
    gw.stop();
    TEST_ASSERT(gw.status() == hft::MdGatewayStatus::Disconnected);
}

void test_fix_trade_stub() {
    hft::FixTradeGateway gw;
    hft::Config cfg;
    gw.init(cfg, "FIX.Trade", nullptr, "fix_account");

    TEST_ASSERT(!gw.is_logged_in());
    TEST_ASSERT(!gw.wait_for_login(1));
    TEST_ASSERT_EQ(gw.get_front_id(), 0);
    TEST_ASSERT_EQ(gw.get_session_id(), 0);

    hft::OrderRequest req{};
    TEST_ASSERT_EQ(gw.send_order(req, "001"), -1);
    TEST_ASSERT_EQ(gw.cancel_order("IF2506", "CFFEX", "001", 0, 0), -1);
    TEST_ASSERT_EQ(gw.query_account(), -1);
    TEST_ASSERT(gw.query_instruments(1).empty());

    gw.stop();
}
