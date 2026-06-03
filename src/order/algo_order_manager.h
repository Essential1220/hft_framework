#pragma once
// ============================================
// algo_order_manager.h — Iceberg + TWAP algorithm order manager
// 冰山单 + TWAP 算法单管理器
// ============================================

#include "common/types.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace hft {

// Algorithm order type (算法单类型)
enum class AlgoOrderType { Iceberg, Twap };
// Algorithm order status (算法单状态)
enum class AlgoOrderStatus { Active, Paused, Completed, Cancelled, Error };

// Algorithm order descriptor (算法单描述符 / 算法订单结构)
struct AlgoOrder {
    uint32_t id = 0;                            // Unique order ID (订单唯一ID)
    AlgoOrderType type = AlgoOrderType::Iceberg; // Iceberg or TWAP (冰山单或TWAP)
    char instrument_id[24]{};                    // Instrument code (合约代码)
    char account_id[16]{};                       // Account ID (资金账号)
    Direction direction = Direction::Buy;        // Buy or Sell (买卖方向)
    Offset offset = Offset::Open;                // Open or Close (开平标志)
    double price = 0.0;                          // Limit price (委托价格)
    int total_volume = 0;                        // Total target volume (总目标手数)
    int filled_volume = 0;                       // Already filled volume (已成交手数)
    int display_volume = 1;      // Iceberg: per-slice visible volume (冰山单：每片可见数量)
    int num_slices = 1;          // TWAP: number of slices (TWAP：总片数)
    int duration_seconds = 60;   // TWAP: total duration (TWAP：总时长/秒)
    AlgoOrderStatus status = AlgoOrderStatus::Active; // Current status (当前状态)
    std::vector<std::string> child_order_refs;         // Child order references (子单报单引用列表)
    std::chrono::steady_clock::time_point created_at;  // Creation timestamp (创建时间)
    std::chrono::steady_clock::time_point last_slice_at; // Last slice sent time (上次发片时间)
    int slices_sent = 0;                               // Number of slices sent so far (已发送片数)
};

using AlgoSendFn = std::function<SendOrderResult(const OrderRequest&)>;

class AlgoOrderManager {
public:
    AlgoOrderManager() = default;

    uint32_t create_iceberg(const std::string& instrument, const std::string& account_id,
                            Direction dir, Offset offset, double price,
                            int total_volume, int display_volume,
                            AlgoSendFn send_fn);

    uint32_t create_twap(const std::string& instrument, const std::string& account_id,
                         Direction dir, Offset offset, double price,
                         int total_volume, int num_slices, int duration_seconds,
                         AlgoSendFn send_fn);

    void cancel(uint32_t id);

    void on_trade(const std::string& order_ref, int filled_volume);

    // Called periodically (e.g. in consumer_loop) to send next TWAP slices
    // 周期性调用（如 consumer_loop 中）发送下一片 TWAP (定时驱动)
    void tick(AlgoSendFn send_fn);

    std::vector<AlgoOrder> get_active() const;
    std::vector<AlgoOrder> get_all() const;

private:
    void send_next_iceberg_slice(AlgoOrder& order, AlgoSendFn& send_fn);
    void send_next_twap_slice(AlgoOrder& order, AlgoSendFn& send_fn);

    std::vector<AlgoOrder> orders_;
    uint32_t next_id_ = 1;
    mutable std::mutex mtx_;
};

} // namespace hft
