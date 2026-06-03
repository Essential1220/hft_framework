// ============================================
// algo_order_manager.cpp — Iceberg + TWAP algorithm execution
// 冰山单 + TWAP 算法单执行实现
// ============================================

#include "order/algo_order_manager.h"
#include <algorithm>

namespace hft {

uint32_t AlgoOrderManager::create_iceberg(
    const std::string& instrument, const std::string& account_id,
    Direction dir, Offset offset, double price,
    int total_volume, int display_volume,
    AlgoSendFn send_fn) {

    std::lock_guard<std::mutex> lock(mtx_);
    AlgoOrder order;
    order.id = next_id_++;
    order.type = AlgoOrderType::Iceberg;
    safe_copy(order.instrument_id, instrument.c_str(), sizeof(order.instrument_id));
    safe_copy(order.account_id, account_id.c_str(), sizeof(order.account_id));
    order.direction = dir;
    order.offset = offset;
    order.price = price;
    order.total_volume = total_volume;
    order.filled_volume = 0;
    order.display_volume = std::max(1, display_volume);
    order.status = AlgoOrderStatus::Active;
    order.created_at = std::chrono::steady_clock::now();
    order.last_slice_at = order.created_at;

    send_next_iceberg_slice(order, send_fn);
    orders_.push_back(std::move(order));
    return orders_.back().id;
}

uint32_t AlgoOrderManager::create_twap(
    const std::string& instrument, const std::string& account_id,
    Direction dir, Offset offset, double price,
    int total_volume, int num_slices, int duration_seconds,
    AlgoSendFn send_fn) {

    std::lock_guard<std::mutex> lock(mtx_);
    AlgoOrder order;
    order.id = next_id_++;
    order.type = AlgoOrderType::Twap;
    safe_copy(order.instrument_id, instrument.c_str(), sizeof(order.instrument_id));
    safe_copy(order.account_id, account_id.c_str(), sizeof(order.account_id));
    order.direction = dir;
    order.offset = offset;
    order.price = price;
    order.total_volume = total_volume;
    order.filled_volume = 0;
    order.num_slices = std::max(1, num_slices);
    order.duration_seconds = std::max(1, duration_seconds);
    order.status = AlgoOrderStatus::Active;
    order.created_at = std::chrono::steady_clock::now();
    order.last_slice_at = order.created_at;
    order.slices_sent = 0;

    // Send first TWAP slice immediately (立即发送第一片 TWAP)
    send_next_twap_slice(order, send_fn);
    orders_.push_back(std::move(order));
    return orders_.back().id;
}

void AlgoOrderManager::cancel(uint32_t id) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& order : orders_) {
        if (order.id == id && order.status == AlgoOrderStatus::Active) {
            order.status = AlgoOrderStatus::Cancelled;
            break;
        }
    }
}

void AlgoOrderManager::on_trade(const std::string& order_ref, int filled_volume) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& order : orders_) {
        if (order.status != AlgoOrderStatus::Active) continue;
        for (const auto& ref : order.child_order_refs) {
            if (ref == order_ref) {
                order.filled_volume += filled_volume;
                if (order.filled_volume >= order.total_volume) {
                    order.status = AlgoOrderStatus::Completed;
                }
                return;
            }
        }
    }
}

void AlgoOrderManager::tick(AlgoSendFn send_fn) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = std::chrono::steady_clock::now();

    for (auto& order : orders_) {
        if (order.status != AlgoOrderStatus::Active) continue;
        if (order.filled_volume >= order.total_volume) {
            order.status = AlgoOrderStatus::Completed;
            continue;
        }

        if (order.type == AlgoOrderType::Twap) {
            if (order.slices_sent >= order.num_slices) {
                // All slices sent, wait for fills (所有片已发送，等待成交)
                continue;
            }
            int interval_ms = (order.duration_seconds * 1000) / order.num_slices;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - order.last_slice_at).count();
            if (elapsed >= interval_ms) {
                send_next_twap_slice(order, send_fn);
            }
        }
        // Iceberg slices are triggered by on_trade fills, not by tick
        // 冰山单通过成交回调触发下一片，不由 tick 定时触发
    }
}

void AlgoOrderManager::send_next_iceberg_slice(AlgoOrder& order, AlgoSendFn& send_fn) {
    int remaining = order.total_volume - order.filled_volume;
    if (remaining <= 0) {
        order.status = AlgoOrderStatus::Completed;
        return;
    }

    int slice_vol = std::min(order.display_volume, remaining);

    OrderRequest req{};
    safe_copy(req.instrument_id, order.instrument_id, sizeof(req.instrument_id));
    safe_copy(req.account_id, order.account_id, sizeof(req.account_id));
    safe_copy(req.strategy_id, "algo_iceberg", sizeof(req.strategy_id));
    req.direction = order.direction;
    req.offset = order.offset;
    req.price = order.price;
    req.volume = slice_vol;

    auto result = send_fn(req);
    if (result.success()) {
        order.child_order_refs.push_back(result.order_ref);
        order.last_slice_at = std::chrono::steady_clock::now();
    } else {
        order.status = AlgoOrderStatus::Error;
    }
}

void AlgoOrderManager::send_next_twap_slice(AlgoOrder& order, AlgoSendFn& send_fn) {
    int remaining = order.total_volume - order.filled_volume;
    int remaining_slices = order.num_slices - order.slices_sent;
    if (remaining <= 0 || remaining_slices <= 0) {
        if (remaining <= 0) order.status = AlgoOrderStatus::Completed;
        return;
    }

    int slice_vol = remaining / remaining_slices;
    if (slice_vol <= 0) slice_vol = 1;
    if (slice_vol > remaining) slice_vol = remaining;

    OrderRequest req{};
    safe_copy(req.instrument_id, order.instrument_id, sizeof(req.instrument_id));
    safe_copy(req.account_id, order.account_id, sizeof(req.account_id));
    safe_copy(req.strategy_id, "algo_twap", sizeof(req.strategy_id));
    req.direction = order.direction;
    req.offset = order.offset;
    req.price = order.price;
    req.volume = slice_vol;

    auto result = send_fn(req);
    if (result.success()) {
        order.child_order_refs.push_back(result.order_ref);
        order.slices_sent++;
        order.last_slice_at = std::chrono::steady_clock::now();
    } else {
        order.status = AlgoOrderStatus::Error;
    }
}

std::vector<AlgoOrder> AlgoOrderManager::get_active() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<AlgoOrder> result;
    for (const auto& order : orders_) {
        if (order.status == AlgoOrderStatus::Active) {
            result.push_back(order);
        }
    }
    return result;
}

std::vector<AlgoOrder> AlgoOrderManager::get_all() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return orders_;
}

} // namespace hft
