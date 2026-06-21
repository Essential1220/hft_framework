#include "engine/tick_reader.h"
#include "engine/backtest_report.h"
#include "engine/backtest_engine.h"
#include "common/binary_io.h"
#include "common/types.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
using namespace hft::binary_io;

static void write_test_htick(const std::string& path, const std::vector<TickData>& ticks) {
    std::ofstream fs(path, std::ios::binary);
    fs.write("HFTTICK1", 8);
    for (const auto& t : ticks) {
        write_string(fs, t.instrument_id);
        write_string(fs, t.exchange_id);
        write_string(fs, t.update_time);
        write_string(fs, t.trading_day);
        write_string(fs, t.action_day);
        write_varuint(fs, static_cast<uint64_t>(t.update_millisec));
        write_varint(fs, fixed_price(t.last_price));
        write_varint(fs, fixed_price(t.pre_close_price));
        write_varint(fs, fixed_price(t.open_price));
        write_varint(fs, fixed_price(t.highest_price));
        write_varint(fs, fixed_price(t.lowest_price));
        write_varuint(fs, static_cast<uint64_t>(t.volume));
        write_varint(fs, fixed_turnover(t.turnover));
        write_varint(fs, fixed_turnover(t.open_interest));
        for (int i = 0; i < kMarketDepth; ++i) {
            write_varint(fs, fixed_price(t.bid[i].price));
            write_varuint(fs, static_cast<uint64_t>(t.bid[i].volume));
            write_varint(fs, fixed_price(t.ask[i].price));
            write_varuint(fs, static_cast<uint64_t>(t.ask[i].volume));
        }
        write_varint(fs, fixed_price(t.upper_limit));
        write_varint(fs, fixed_price(t.lower_limit));
    }
}

static TickData make_tick(const char* instrument, double price, int volume,
                          const char* time, int ms) {
    TickData t{};
    std::strncpy(t.instrument_id, instrument, sizeof(t.instrument_id) - 1);
    std::strncpy(t.exchange_id, "SHFE", sizeof(t.exchange_id) - 1);
    std::strncpy(t.update_time, time, sizeof(t.update_time) - 1);
    std::strncpy(t.trading_day, "20260101", sizeof(t.trading_day) - 1);
    std::strncpy(t.action_day, "20260101", sizeof(t.action_day) - 1);
    t.update_millisec = ms;
    t.last_price = price;
    t.bid[0].price = price - 1.0;
    t.bid[0].volume = 10;
    t.ask[0].price = price + 1.0;
    t.ask[0].volume = 10;
    t.volume = volume;
    t.upper_limit = price + 100.0;
    t.lower_limit = price - 100.0;
    return t;
}

void test_tick_reader_roundtrip() {
    const std::string path = "test_ticks_roundtrip.htick";
    std::vector<TickData> original;
    original.push_back(make_tick("rb2610", 3500.0, 100, "09:00:01", 0));
    original.push_back(make_tick("rb2610", 3501.0, 200, "09:00:01", 500));
    original.push_back(make_tick("rb2610", 3502.0, 300, "09:00:02", 0));

    write_test_htick(path, original);

    std::vector<TickData> loaded;
    TEST_ASSERT(read_htick_file(path, loaded));
    TEST_ASSERT_EQ(loaded.size(), 3u);
    TEST_ASSERT_NEAR(loaded[0].last_price, 3500.0, 0.01);
    TEST_ASSERT_NEAR(loaded[1].last_price, 3501.0, 0.01);
    TEST_ASSERT_NEAR(loaded[2].last_price, 3502.0, 0.01);
    TEST_ASSERT_EQ(loaded[0].update_millisec, 0);
    TEST_ASSERT_EQ(loaded[1].update_millisec, 500);
    TEST_ASSERT_EQ(std::string(loaded[0].instrument_id), "rb2610");

    std::remove(path.c_str());
}

void test_tick_reader_bad_header() {
    const std::string path = "test_ticks_bad.htick";
    {
        std::ofstream fs(path, std::ios::binary);
        fs.write("BADHEADR", 8);
    }
    std::vector<TickData> loaded;
    TEST_ASSERT(!read_htick_file(path, loaded));
    std::remove(path.c_str());
}

void test_tick_reader_empty_file() {
    const std::string path = "test_ticks_empty.htick";
    {
        std::ofstream fs(path, std::ios::binary);
        fs.write("HFTTICK1", 8);
    }
    std::vector<TickData> loaded;
    TEST_ASSERT(read_htick_file(path, loaded));
    TEST_ASSERT_EQ(loaded.size(), 0u);
    std::remove(path.c_str());
}

void test_merge_ticks_by_time() {
    std::vector<TickData> a, b;
    a.push_back(make_tick("rb2610", 3500.0, 100, "09:00:01", 0));
    a.push_back(make_tick("rb2610", 3502.0, 300, "09:00:03", 0));
    b.push_back(make_tick("cu2610", 50000.0, 50, "09:00:02", 0));

    std::vector<std::vector<TickData>> sources = {a, b};
    std::vector<TickData> merged;
    merge_ticks_by_time(sources, merged);

    TEST_ASSERT_EQ(merged.size(), 3u);
    TEST_ASSERT_EQ(std::string(merged[0].update_time), "09:00:01");
    TEST_ASSERT_EQ(std::string(merged[1].update_time), "09:00:02");
    TEST_ASSERT_EQ(std::string(merged[2].update_time), "09:00:03");
}

void test_backtest_report_compute() {
    std::vector<BacktestTrade> trades;

    // Open long 1@3500, Close long 1@3510 = +10
    trades.push_back({"rb2610", Direction::Buy, Offset::Open, 3500.0, 1, "09:00:01"});
    trades.push_back({"rb2610", Direction::Sell, Offset::Close, 3510.0, 1, "09:00:02"});

    // Open short 1@3520, Close short 1@3525 = -5
    trades.push_back({"rb2610", Direction::Sell, Offset::Open, 3520.0, 1, "09:00:03"});
    trades.push_back({"rb2610", Direction::Buy, Offset::Close, 3525.0, 1, "09:00:04"});

    BacktestReport report = compute_backtest_report(trades, 100);
    TEST_ASSERT_EQ(report.total_ticks, 100);
    TEST_ASSERT_EQ(report.total_trades, 4);
    TEST_ASSERT_NEAR(report.total_pnl, 5.0, 0.01);
    TEST_ASSERT_EQ(report.winning_trades, 1);
    TEST_ASSERT_EQ(report.losing_trades, 1);
    TEST_ASSERT_NEAR(report.win_rate, 0.5, 0.01);
}

void test_backtest_report_empty() {
    std::vector<BacktestTrade> trades;
    BacktestReport report = compute_backtest_report(trades, 0);
    TEST_ASSERT_EQ(report.total_trades, 0);
    TEST_ASSERT_NEAR(report.total_pnl, 0.0, 0.01);
}

void test_backtest_context_fill() {
    BacktestContext ctx;
    TickData tick = make_tick("rb2610", 3500.0, 100, "09:00:01", 0);
    ctx.set_current_tick(tick);

    OrderRequest req{};
    std::strncpy(req.instrument_id, "rb2610", sizeof(req.instrument_id));
    std::strncpy(req.exchange_id, "SHFE", sizeof(req.exchange_id));
    std::strncpy(req.account_id, "TEST", sizeof(req.account_id));
    std::strncpy(req.strategy_id, "bt", sizeof(req.strategy_id));
    req.direction = Direction::Buy;
    req.offset = Offset::Open;
    req.price = 3500.0;
    req.volume = 1;

    std::string ref = ctx.send_order_with_ref(req);
    TEST_ASSERT(!ref.empty());
    TEST_ASSERT_EQ(ctx.trades().size(), 1u);
    TEST_ASSERT_NEAR(ctx.trades()[0].price, 3500.0, 0.01);
    TEST_ASSERT_EQ(ctx.get_net_position("rb2610", "TEST"), 1);
}
