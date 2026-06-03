#pragma once
// ============================================
// event.h - Event type definitions (tagged union) (事件类型定义，tagged union)
// Used for SPSC queue to pass between gateway thread and engine consumer thread
// (用于 SPSC 队列在网关线程和引擎消费线程之间传递)
// ============================================

#include "common/types.h"

#include <cstdint>
#include <type_traits>

namespace hft {

enum class EventType : uint8_t {
    Tick,
    Order,
    Trade,
    Account,
    Position,
    CancelRejected
};

struct Event {
    EventType type;
    union {
        TickData tick;
        OrderInfo order;
        TradeInfo trade;
        AccountInfo account;
        PositionInfo position;
        CancelRejectInfo cancel_reject;
    };

    Event() : type(EventType::Tick), tick{} {}
};

static_assert(std::is_trivially_copyable_v<Event>, "Event must be trivially copyable for SPSC queue");

enum class CommandType : uint8_t {
    Pause,
    Resume,
    Stop,
    EmergencyClose,
    AddCondOrder,
    CancelCondOrder,
    UpdateMonitorConfig,
    SetRiskMode          // Async set RMS risk mode on market/trade queue overflow (行情/交易队列溢出时异步设置 RMS 风控模式)
};

struct RuntimeMonitorConfigUpdate {
    int no_tick_warn_seconds = 0;
    char trading_sessions[128]{};
};

struct EngineCommand {
    CommandType type;
    union {
        ConditionalOrder cond_order;
        uint32_t cond_order_id;
        RuntimeMonitorConfigUpdate monitor_config;
        RiskMode risk_mode;   // Target mode for SetRiskMode command (SetRiskMode 命令的目标模式)
    };
    char reason[128]{};       // Reason for SetRiskMode command (SetRiskMode 命令的原因说明)

    EngineCommand() : type(CommandType::Pause), cond_order{} {}
};

static_assert(std::is_trivially_copyable_v<EngineCommand>, "EngineCommand must be trivially copyable for SPSC queue");

} // namespace hft
