#include "engine/tick_reader.h"
#include "common/binary_io.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace hft {

using namespace binary_io;

static bool read_one_tick(std::istream& is, TickData& tick) {
    std::string s;
    uint64_t u;
    int64_t v;

    if (!read_string(is, &s)) return false;
    std::strncpy(tick.instrument_id, s.c_str(), sizeof(tick.instrument_id) - 1);

    if (!read_string(is, &s)) return false;
    std::strncpy(tick.exchange_id, s.c_str(), sizeof(tick.exchange_id) - 1);

    if (!read_string(is, &s)) return false;
    std::strncpy(tick.update_time, s.c_str(), sizeof(tick.update_time) - 1);

    if (!read_string(is, &s)) return false;
    std::strncpy(tick.trading_day, s.c_str(), sizeof(tick.trading_day) - 1);

    if (!read_string(is, &s)) return false;
    std::strncpy(tick.action_day, s.c_str(), sizeof(tick.action_day) - 1);

    if (!read_varuint(is, &u)) return false;
    tick.update_millisec = static_cast<int>(u);

    if (!read_varint(is, &v)) return false;
    tick.last_price = restore_price(v);

    if (!read_varint(is, &v)) return false;
    tick.pre_close_price = restore_price(v);

    if (!read_varint(is, &v)) return false;
    tick.open_price = restore_price(v);

    if (!read_varint(is, &v)) return false;
    tick.highest_price = restore_price(v);

    if (!read_varint(is, &v)) return false;
    tick.lowest_price = restore_price(v);

    if (!read_varuint(is, &u)) return false;
    tick.volume = static_cast<int>(u);

    if (!read_varint(is, &v)) return false;
    tick.turnover = restore_turnover(v);

    if (!read_varint(is, &v)) return false;
    tick.open_interest = restore_turnover(v);

    for (int i = 0; i < kMarketDepth; ++i) {
        if (!read_varint(is, &v)) return false;
        tick.bid[i].price = restore_price(v);
        if (!read_varuint(is, &u)) return false;
        tick.bid[i].volume = static_cast<int>(u);
        if (!read_varint(is, &v)) return false;
        tick.ask[i].price = restore_price(v);
        if (!read_varuint(is, &u)) return false;
        tick.ask[i].volume = static_cast<int>(u);
    }

    if (!read_varint(is, &v)) return false;
    tick.upper_limit = restore_price(v);

    if (!read_varint(is, &v)) return false;
    tick.lower_limit = restore_price(v);

    return true;
}

bool read_htick_file(const std::string& filepath, std::vector<TickData>& out) {
    std::ifstream fs(filepath, std::ios::binary);
    if (!fs.is_open()) return false;

    char header[8];
    if (!fs.read(header, 8) || std::strncmp(header, "HFTTICK1", 8) != 0) {
        return false;
    }

    while (fs.peek() != EOF) {
        TickData tick{};
        if (!read_one_tick(fs, tick)) break;
        out.push_back(tick);
    }

    return !out.empty() || fs.eof();
}

static int tick_time_cmp(const TickData& a, const TickData& b) {
    int c = std::strcmp(a.update_time, b.update_time);
    if (c != 0) return c;
    return a.update_millisec - b.update_millisec;
}

void merge_ticks_by_time(std::vector<std::vector<TickData>>& sources,
                         std::vector<TickData>& merged) {
    merged.clear();
    size_t total = 0;
    for (const auto& src : sources) total += src.size();
    merged.reserve(total);

    for (auto& src : sources) {
        merged.insert(merged.end(), src.begin(), src.end());
    }

    std::stable_sort(merged.begin(), merged.end(),
        [](const TickData& a, const TickData& b) {
            return tick_time_cmp(a, b) < 0;
        });
}

} // namespace hft
