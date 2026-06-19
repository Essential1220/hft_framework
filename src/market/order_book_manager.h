#pragma once

#include "common/types.h"
#include "market/order_book.h"

#include <shared_mutex>
#include <unordered_map>

namespace hft {

class OrderBookManager {
public:
    void update(const TickData& tick) {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        auto& book = books_[InstrumentKey(tick.instrument_id)];
        book.update_from_tick(tick);
    }

    WindowedOrderBook get_book(const char* instrument) const {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        auto it = books_.find(InstrumentKey(instrument));
        if (it != books_.end()) return it->second;
        return {};
    }

    bool has_book(const char* instrument) const {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        return books_.find(InstrumentKey(instrument)) != books_.end();
    }

private:
    mutable std::shared_mutex mtx_;
    std::unordered_map<InstrumentKey, WindowedOrderBook, InstrumentKeyHash> books_;
};

} // namespace hft
