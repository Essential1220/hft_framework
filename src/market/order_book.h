#pragma once

#include "common/types.h"

#include <cmath>
#include <cstring>

namespace hft {

class WindowedOrderBook {
public:
    static constexpr int kDepth = kMarketDepth;

    void update_from_tick(const TickData& tick) {
        std::memcpy(instrument_id_, tick.instrument_id, sizeof(instrument_id_));
        for (int i = 0; i < kDepth; ++i) {
            bids_[i] = tick.bid[i];
            asks_[i] = tick.ask[i];
        }
        last_price_ = tick.last_price;
        update_ns_ = tick.local_recv_ns;
    }

    double mid_price() const {
        if (bids_[0].price <= 0.0 || asks_[0].price <= 0.0) return last_price_;
        return (bids_[0].price + asks_[0].price) * 0.5;
    }

    double weighted_mid() const {
        if (bids_[0].price <= 0.0 || asks_[0].price <= 0.0) return mid_price();
        const double total = bids_[0].volume + asks_[0].volume;
        if (total <= 0.0) return mid_price();
        return (bids_[0].price * asks_[0].volume + asks_[0].price * bids_[0].volume) / total;
    }

    double bid_ask_spread() const {
        if (bids_[0].price <= 0.0 || asks_[0].price <= 0.0) return 0.0;
        return asks_[0].price - bids_[0].price;
    }

    double bid_ask_imbalance() const {
        const double total = bids_[0].volume + asks_[0].volume;
        if (total <= 0.0) return 0.0;
        return static_cast<double>(bids_[0].volume - asks_[0].volume) / total;
    }

    double vwap(int depth) const {
        if (depth <= 0) depth = 1;
        if (depth > kDepth) depth = kDepth;
        double sum_pv = 0.0;
        int sum_v = 0;
        for (int i = 0; i < depth; ++i) {
            if (asks_[i].price > 0.0 && asks_[i].volume > 0) {
                sum_pv += asks_[i].price * asks_[i].volume;
                sum_v += asks_[i].volume;
            }
        }
        return sum_v > 0 ? sum_pv / sum_v : 0.0;
    }

    const PriceLevel& bid(int level) const {
        static const PriceLevel empty{};
        return (level >= 0 && level < kDepth) ? bids_[level] : empty;
    }

    const PriceLevel& ask(int level) const {
        static const PriceLevel empty{};
        return (level >= 0 && level < kDepth) ? asks_[level] : empty;
    }

    const char* instrument_id() const { return instrument_id_; }
    int64_t update_time_ns() const { return update_ns_; }
    double last_price() const { return last_price_; }

    int total_bid_volume(int depth) const {
        if (depth <= 0) depth = 1;
        if (depth > kDepth) depth = kDepth;
        int total = 0;
        for (int i = 0; i < depth; ++i) total += bids_[i].volume;
        return total;
    }

    int total_ask_volume(int depth) const {
        if (depth <= 0) depth = 1;
        if (depth > kDepth) depth = kDepth;
        int total = 0;
        for (int i = 0; i < depth; ++i) total += asks_[i].volume;
        return total;
    }

private:
    char instrument_id_[24]{};
    PriceLevel bids_[kDepth]{};
    PriceLevel asks_[kDepth]{};
    double last_price_ = 0.0;
    int64_t update_ns_ = 0;
};

} // namespace hft
