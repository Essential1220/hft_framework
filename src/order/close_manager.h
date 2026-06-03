#pragma once
// ============================================
// close_manager.h - Close position manager (平仓管理器 / 强平任务调度)
//
// Manages forced-close (liquidation) tasks with retry, timeout, and state machine.
// 管理强制平仓任务，支持重试、超时和状态机调度。
// ============================================

#include "common/types.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace hft {

// Close task state machine (强平任务状态机 / 平仓任务状态机):
// Pending -> Sent -> Cancelling -> Pending/Done/Failed
// Pending -> OrderRejected -> Pending/Failed
enum class CloseTaskState : uint8_t {
    Pending,        // Waiting to be sent (等待发送)
    Sent,           // Order has been sent to exchange (已发送至交易所)
    OrderRejected,  // Order was rejected by exchange (订单被交易所拒绝)
    Cancelling,     // Cancel request in progress (撤单进行中)
    Done,           // Task completed successfully (已完成)
    Failed          // Task failed, e.g. retries exhausted (失败，如重试次数耗尽)
};

// Close position task descriptor (平仓任务结构体 / 强平任务描述)
struct CloseTask {
    uint32_t id = 0;                // Unique task ID (任务ID / 任务唯一标识)
    char instrument_id[24]{};       // Instrument code (合约代码)
    char exchange_id[8]{};          // Exchange code (交易所代码)
    char account_id[16]{};          // Account ID (资金账号)
    Direction pos_direction = Direction::Buy; // Position direction being closed (持仓方向，即被平仓的方向)
    Offset offset = Offset::Close;  // Close type: CloseToday / CloseYesterday / Close (平仓类型：平今/平昨/平仓)
    int target_volume = 0;          // Target volume to close (目标平仓数量 / 目标平仓手数)
    int filled_volume = 0;          // Volume already filled (已平仓数量 / 已成交手数)
    int retry_count = 0;            // Number of retries attempted (重试次数)
    double last_price = 0.0;        // Last traded price, used for close price calculation (最新价，用于计算平仓价格)
    double upper_limit = 0.0;       // Upper limit price (涨停价)
    double lower_limit = 0.0;       // Lower limit price (跌停价)
    std::string order_ref;          // Associated order reference (关联的报单引用)
    CloseTaskState state = CloseTaskState::Pending; // Current task state (任务状态)
    std::chrono::steady_clock::time_point sent_time; // Time when order was sent (发送时间)
};

// Close position manager: orchestrates forced-close (liquidation) tasks
// 平仓管理器：编排强制平仓（强平）任务 (平仓管理器)
class CloseManager {
public:
    static constexpr int kMaxRetry = 3;         // Max retry count (最大重试次数)
    static constexpr int kTimeoutSeconds = 5;   // Task timeout in seconds (任务超时时间/秒)

    // Callback function type definitions (回调函数类型定义)
    using SendOrderFunc = std::function<void(const OrderRequest&, std::string&)>;
    using CancelOrderFunc = std::function<bool(const std::string&, const std::string&)>;
    using AlertFunc = std::function<void(const std::string&)>;

    // Set the three callback functions (设置回调函数)
    void set_callbacks(SendOrderFunc send_fn, CancelOrderFunc cancel_fn, AlertFunc alert_fn);

    // Submit a new close position request (提交新的平仓请求 / 提交强平)
    void submit_close(const char* instrument, const char* exchange_id, const char* account_id,
                      Direction pos_direction, int today_vol, int yesterday_vol,
                      double last_price, double upper_limit, double lower_limit);

    // Handle order status update from exchange (处理报单状态更新 / 订单回调)
    void on_order(const OrderInfo& order);
    // Handle trade fill notification from exchange (处理成交通知 / 成交回调)
    void on_trade(const TradeInfo& trade);
    // Handle the case where a cancel request was rejected by the exchange (处理撤单被拒绝的情况 / 撤单拒绝回调)
    void on_cancel_rejected(const char* account_id, const char* order_ref, const char* reason);

    // Check for timed-out close tasks and trigger cancel / retry (检查并处理超时的平仓任务 / 超时检查)
    void check_timeout();
    // Remove completed tasks from the task list (清理已完成的任务 / 清理完成)
    void cleanup_finished_tasks();
    // Reconcile task state against a snapshot of active orders from the exchange (依据查询到的活动委托快照核对任务状态 / 任务对账)
    void reconcile_active_orders(const std::string& account_id, const std::vector<OrderInfo>& active_orders);

    // Check whether there are any tasks still pending (检查是否有仍在挂起的任务 / 挂起检查)
    bool has_pending_tasks() const;
    // Get a snapshot of all current close tasks (获取当前所有的平仓任务快照 / 快照)
    std::vector<CloseTask> get_tasks() const;
    // Manually retry a failed task that still has remaining volume (手动重试已失败但有剩余手数的任务 / 手动重试)
    bool retry_failed_task(uint32_t task_id);

    // During startup recovery, only restore task state without sending orders,
    // 启动恢复时只恢复任务状态，不立即发单，
    // to avoid triggering orders before the trading link is ready.
    // 避免在交易未就绪前误触发。 (恢复任务)
    void restore_tasks(const std::vector<CloseTask>& tasks);

    // Re-submit pending tasks after the trading link is fully ready.
    // 在交易链路恢复就绪后重新投递未完成任务。 (恢复投递)
    void resume_pending_tasks();

private:
    // Insert or update a close task while holding the lock (在持锁状态下插入或更新平仓任务 / 持锁更新任务)
    void upsert_task_locked(const char* instrument, const char* exchange_id, const char* account_id,
                            Direction pos_direction, Offset offset, int requested_volume,
                            double last_price, double upper_limit, double lower_limit);
    // Internal: send a specific close task to the exchange (内部方法：发送具体的平仓任务 / 发送任务)
    void send_task(uint32_t task_id);
    // Handle task retry logic (任务重试逻辑处理 / 重试逻辑)
    void retry_task(CloseTask& task, const std::string& reason);
    // Calculate the order price for the close task:
    // use lower_limit for closing longs, upper_limit for closing shorts to guarantee fill.
    // 根据任务信息计算报单价格（平多用跌停价，平空用涨停价以确保成交）(计算平仓价)
    double calc_close_price(const CloseTask& task) const;
    // Internal: clean up completed tasks while holding the lock (内部方法：在持锁状态下清理已完成的任务 / 持锁清理)
    void cleanup_finished_tasks_locked();
    // Determine whether the given order belongs to the specified active close task (判断该报单是否属于当前活动的平仓任务 / 委托匹配)
    bool is_same_active_close_order(const CloseTask& task, const OrderInfo& order) const;

    std::vector<CloseTask> tasks_;          // Close task list (平仓任务列表)
    uint32_t next_id_ = 1;                  // Next task ID allocator (下一个任务 ID 分配器)
    mutable std::mutex mtx_;                // Mutex protecting the task list (保护任务列表的互斥锁)

    SendOrderFunc send_fn_;                 // Send order callback (发送订单回调函数)
    CancelOrderFunc cancel_fn_;             // Cancel order callback (撤销订单回调函数)
    AlertFunc alert_fn_;                    // Alert message callback (告警信息回调函数)
};

} // namespace hft
