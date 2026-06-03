#pragma once
// ============================================
// account_context.h - Single account context (单个账户的上下文)
// Each account owns an independent trade gateway, order manager, and position manager.
// (每个账户拥有独立的交易网关、委托管理器、持仓管理器)
// ============================================

#include "common/types.h"
#include "common/spsc_queue.h"
#include "common/event.h"
#include "gateway/i_trade_gateway.h"
#include "order/order_manager.h"
#include "position/position_manager.h"
#include "risk/risk_manager.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace hft {

struct AccountContext {
    std::string account_id;         // Account ID, e.g. "Account1"; empty means default account (账户ID, 空串表示默认账户)
    std::string config_section;     // Corresponding config section, e.g. "CTP", "CTP.Account1" (对应的配置节)
    std::string gateway_type;       // Gateway type, e.g. "CTP", used for factory dispatch (网关类型, 用于工厂分发)

    std::unique_ptr<ITradeGateway> trade_gateway; // Independent trade gateway for this account (该账户独立的交易网关)
    OrderManager order_mgr;                       // Independent order manager for this account (该账户独立的委托管理器)
    PositionManager position_mgr;                 // Independent position manager for this account (该账户独立的持仓管理器)
    RiskManager risk_mgr;                         // Independent risk control manager for this account (该账户独立的风控管理器)
    AccountInfo account_info{};                   // Account fund info for this account (该账户的资金信息)
    mutable std::mutex account_mtx;               // Mutex protecting account_info (保护 account_info 的互斥锁)

    // Startup snapshot sync state — used to judge whether data sync is complete on boot/reconnect
    // (启动快照同步状态 — 用于判断启动或重连时数据是否同步完成)
    bool account_snapshot_ready = false;          // Whether account fund snapshot is ready (资金快照是否就绪)
    bool position_snapshot_ready = false;         // Whether position snapshot is ready (持仓快照是否就绪)
    bool active_orders_snapshot_ready = false;    // Whether active order snapshot is ready (活动委托快照是否就绪)
    bool reconnect_sync_pending = false;          // Whether reconnection sync is in progress (是否正在进行重连同步)
    bool resume_after_reconnect = false;          // Whether to resume strategy after reconnect completes (重连完成后是否需要恢复策略运行)
    std::chrono::steady_clock::time_point reconnect_start_time;  // Reconnect start time (重连开始时间)
    AccountTradeState trade_state = AccountTradeState::Unknown;

    // Per-account independent trade event queue (SPSC: lock-free single-producer single-consumer queue)
    // Used to pass events from CTP trade callback thread to the engine consumer thread.
    // (每个账户独立的交易事件队列，SPSC 无锁单生产者单消费者队列，用于将 CTP 交易回调线程的事件传递到引擎消费线程)
    mutable std::mutex reject_mtx;                // Protects the latest local reject details (保护最新的本地拒单细节)
    OrderRejectReason last_reject_reason = OrderRejectReason::None;
    std::string last_reject_message;
    static constexpr size_t kQueueSize = 8192;
    SPSCQueue<Event, kQueueSize> trade_queue;
};

} // namespace hft
