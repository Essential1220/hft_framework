#pragma once
// ============================================
// order_manager.h - Order manager (zero-alloc hot path)
// 订单管理器（零分配热路径）
// Uses FixedKey<16> instead of std::string for order_ref lookups
// 使用 FixedKey<16> 替代 std::string 做 order_ref 查找
// ============================================

#include "common/types.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <vector>

namespace hft {

class OrderManager {
public:
    void init(int front_id, int session_id, int max_order_ref);
    std::string allocate_order_ref();
    OrderInfo create_order(const OrderRequest& req);
    void on_order_return(const OrderInfo& order);
    void on_trade_return(const TradeInfo& trade);
    void replace_active_orders(const std::vector<OrderInfo>& active_orders);
    void remove_order(const std::string& order_ref);
    bool get_order_copy(const std::string& order_ref, OrderInfo& out_order) const;
    bool enrich_order_info(OrderInfo& order) const;
    bool enrich_trade_info(TradeInfo& trade) const;
    std::vector<OrderInfo> get_active_orders() const;
    int get_pending_open_volume(const char* instrument, Direction dir) const;
    int get_pending_close_volume(const char* instrument, Direction pos_dir, Offset offset) const;

    int get_front_id() const { return front_id_; }
    int get_session_id() const { return session_id_; }

private:
    bool is_active_order(const OrderInfo& order) const;
    // Compute the volume an order contributes to the pending_open counter (non-zero only for Open + active)
    // 计算订单贡献给 pending_open 计数器的体量（open + active 时才非零）(计算待开仓量)
    static int open_remaining_of(const OrderInfo& order);
    // Composite key (instrument, dir) for O(1) pending_open accumulation.
    // 复合键 (instrument, dir)，用于 O(1) 累加 pending_open。 (复合键)
    struct PendingOpenKey {
        InstrumentKey instrument;
        Direction dir = Direction::Buy;
        bool operator==(const PendingOpenKey& other) const {
            return dir == other.dir && instrument == other.instrument;
        }
    };
    struct PendingOpenKeyHash {
        size_t operator()(const PendingOpenKey& k) const noexcept {
            return InstrumentKeyHash{}(k.instrument) ^ (static_cast<size_t>(k.dir) * 0x9E3779B1u);
        }
    };
    // Note: all pending_open_ mutations must complete while holding mtx_.
    // 注意：所有 pending_open_ 变化必须在 mtx_ 持有期间完成。
    // Before/after order states are summed via open_remaining_of(); delta is accumulated into the table.
    // 调用前后的订单状态都用 open_remaining_of() 求和，delta 累加到表。 (累加delta)
    void apply_pending_open_delta(const OrderInfo* before, const OrderInfo* after);
    void rebuild_pending_open();

    using OrderMap = std::unordered_map<OrderRefKey, OrderInfo, FixedKeyHash<16>>;
    OrderMap orders_;
    mutable std::mutex mtx_;
    std::atomic<int> order_ref_{0};
    int front_id_ = 0;
    int session_id_ = 0;

    // Incremental pending_open counter, maintained under mtx_; O(1) reads instead of O(N).
    // 增量 pending_open 计数器，在 mtx_ 下维护；读端 O(1) 而非 O(N)。 (增量计数器)
    std::unordered_map<PendingOpenKey, int, PendingOpenKeyHash> pending_open_by_inst_dir_;

    // CTP trade deduplication: prevent THOST_TERT_QUICK replay from causing duplicate fills
    // CTP 成交去重：防止 THOST_TERT_QUICK 重放导致重复成交 (成交去重)
    using TradeIdKey = FixedKey<24>;
    std::unordered_set<TradeIdKey, FixedKeyHash<24>> seen_trade_ids_;
};

} // namespace hft
