#pragma once
// ============================================
// types.h - Core POD data structures (核心 POD 数据结构)
// ============================================

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>

namespace hft {

inline void safe_copy(char* dst, const char* src, size_t max_len) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, max_len - 1);
    dst[max_len - 1] = '\0';
}

inline bool str_equal(const char* a, const char* b) {
    return std::strcmp(a, b) == 0;
}

enum class Direction { Buy, Sell };

enum class Offset {
    Open,
    Close,
    CloseToday,     // Close today (平今)
    CloseYesterday  // Close yesterday (平昨)
};

// Zero-allocation fixed-size key for hot path maps (热路径零分配固定大小键)
struct InstrumentKey {
    char data[20]{};

    InstrumentKey() = default;
    explicit InstrumentKey(const char* s) { safe_copy(data, s, sizeof(data)); }
    InstrumentKey(const char* instrument, Direction dir) {
        size_t len = 0;
        while (instrument[len] && len < 15) { data[len] = instrument[len]; ++len; }
        data[len++] = '_';
        data[len++] = (dir == Direction::Buy) ? 'L' : 'S';
        data[len] = '\0';
    }
    InstrumentKey(const char* instrument, const char* suffix) {
        size_t len = 0;
        while (instrument[len] && len < 16) { data[len] = instrument[len]; ++len; }
        size_t i = 0;
        while (suffix[i] && len < sizeof(data) - 1) { data[len++] = suffix[i++]; }
        data[len] = '\0';
    }
    bool operator==(const InstrumentKey& o) const { return std::strcmp(data, o.data) == 0; }
    bool operator<(const InstrumentKey& o) const { return std::strcmp(data, o.data) < 0; }
};

struct InstrumentKeyHash {
    size_t operator()(const InstrumentKey& k) const {
        size_t h = 14695981039346656037ULL;
        for (const char* p = k.data; *p; ++p) {
            h ^= static_cast<size_t>(static_cast<unsigned char>(*p));
            h *= 1099511628211ULL;
        }
        return h;
    }
};

inline uint32_t instrument_hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= static_cast<uint32_t>(static_cast<unsigned char>(*s));
        h *= 16777619u;
    }
    return h;
}

template <size_t N>
struct FixedKey {
    char data[N]{};
    FixedKey() = default;
    explicit FixedKey(const char* s) { safe_copy(data, s, N); }
    bool operator==(const FixedKey& o) const { return std::strcmp(data, o.data) == 0; }
    bool operator<(const FixedKey& o) const { return std::strcmp(data, o.data) < 0; }
};

using OrderRefKey = FixedKey<16>;

template <size_t N>
struct FixedKeyHash {
    size_t operator()(const FixedKey<N>& k) const {
        size_t h = 14695981039346656037ULL;
        for (const char* p = k.data; *p; ++p) {
            h ^= static_cast<size_t>(static_cast<unsigned char>(*p));
            h *= 1099511628211ULL;
        }
        return h;
    }
};

struct InstrumentSpec {
    std::string instrument_id;
    std::string instrument_name;
    std::string exchange_id;
    std::string product_id;
    std::string underlying_instrument_id;
    std::string expire_date;
    std::string start_deliv_date;
    std::string end_deliv_date;
    char inst_life_phase = '\0';
    bool is_trading = false;
    double strike_price = 0.0;
    char product_class = '\0';
    char options_type = '\0';
    double price_tick = 0.2;
    int volume_multiple = 1;
    double long_margin_ratio = 0.12;
    double short_margin_ratio = 0.12;
    double open_commission_rate = 0.00002;
    double close_commission_rate = 0.00002;
    double close_today_commission_rate = 0.00002;
};

enum class OrderStatus {
    Pending,
    PartTraded,
    AllTraded,
    Cancelled,
    Error,
    // v2: new states appended after existing values (v2: 新状态追加在已有值之后)
    Created,
    RiskRejected,
    CancelPending,
    Submitted,
};

inline OrderStatus to_legacy_status(OrderStatus s) {
    switch (s) {
        case OrderStatus::Created:       return OrderStatus::Pending;
        case OrderStatus::RiskRejected:  return OrderStatus::Error;
        case OrderStatus::CancelPending: return OrderStatus::Pending;
        case OrderStatus::Submitted:     return OrderStatus::Pending;
        default:                         return s;
    }
}

enum class AccountTradeState {
    Unknown,
    Booting,
    LoginPending,
    SnapshotSync,
    Ready,
    ReconnectSync,
    GatewayDown,
    Stopped
};

enum class OrderRejectReason {
    None,
    EngineNotReady,
    UnknownAccount,
    AccountNotReady,
    ReconnectSync,
    GatewayDisconnected,
    GatewayLoginFailed,
    PositionUnavailable,
    RiskCheckFailed,
    GatewaySendFailed
};

inline const char* to_string(AccountTradeState state) {
    switch (state) {
        case AccountTradeState::Unknown: return "unknown";
        case AccountTradeState::Booting: return "booting";
        case AccountTradeState::LoginPending: return "login_pending";
        case AccountTradeState::SnapshotSync: return "snapshot_sync";
        case AccountTradeState::Ready: return "ready";
        case AccountTradeState::ReconnectSync: return "reconnect_sync";
        case AccountTradeState::GatewayDown: return "gateway_down";
        case AccountTradeState::Stopped: return "stopped";
    }
    return "unknown";
}

inline const char* to_string(OrderRejectReason reason) {
    switch (reason) {
        case OrderRejectReason::None: return "none";
        case OrderRejectReason::EngineNotReady: return "engine_not_ready";
        case OrderRejectReason::UnknownAccount: return "unknown_account";
        case OrderRejectReason::AccountNotReady: return "account_not_ready";
        case OrderRejectReason::ReconnectSync: return "reconnect_sync";
        case OrderRejectReason::GatewayDisconnected: return "gateway_disconnected";
        case OrderRejectReason::GatewayLoginFailed: return "gateway_login_failed";
        case OrderRejectReason::PositionUnavailable: return "position_unavailable";
        case OrderRejectReason::RiskCheckFailed: return "risk_check_failed";
        case OrderRejectReason::GatewaySendFailed: return "gateway_send_failed";
    }
    return "none";
}

// Market depth level: one price + one volume (盘口档位：一个价格 + 一个量)
struct PriceLevel {
    double price = 0.0;
    int volume = 0;
};

static constexpr int kMarketDepth = 5;

struct TickData {
    char instrument_id[24]{};
    char exchange_id[8]{};
    double last_price = 0.0;
    double pre_close_price = 0.0;
    double open_price = 0.0;
    double highest_price = 0.0;
    double lowest_price = 0.0;
    int volume = 0;
    double turnover = 0.0;
    double open_interest = 0.0;
    PriceLevel bid[kMarketDepth]{};  // bid[0] = best bid (买一), bid[4] = 5th bid (买五)
    PriceLevel ask[kMarketDepth]{};  // ask[0] = best ask (卖一), ask[4] = 5th ask (卖五)
    double upper_limit = 0.0;
    double lower_limit = 0.0;
    char update_time[12]{};
    int update_millisec = 0;
    char trading_day[12]{};
    char action_day[12]{};
    int64_t local_recv_ns = 0;  // Local nanosecond timestamp when tick was received (steady_clock)
    // (本机收到 tick 的纳秒时间戳，steady_clock)

    // Compatibility accessors: preserve read-only shortcuts for bid_price1/ask_price1 etc.
    // Avoids modifying large amounts of legacy code; new code should use bid[0].price for clarity.
    // (兼容访问器：保留常用字段的只读快捷方式，避免大量旧代码逐行修改，新代码用 bid[0].price 更清晰)
    double bid_price1() const { return bid[0].price; }
    int    bid_volume1() const { return bid[0].volume; }
    double ask_price1() const { return ask[0].price; }
    int    ask_volume1() const { return ask[0].volume; }
};


struct OrderRequest {
    enum class PriceType {
        Limit,
        Market,
        Fak
    };

    char instrument_id[24]{};
    char exchange_id[8]{};
    char account_id[16]{};
    char strategy_id[32]{};
    Direction direction = Direction::Buy;
    PriceType price_type = PriceType::Limit;
    Offset offset = Offset::Open;
    double price = 0.0;
    int volume = 0;
};

struct SendOrderResult {
    std::string order_ref;       // Non-empty = success (非空=成功)
    std::string reject_reason;   // Non-empty = rejected (非空=被拒)
    std::string reject_message;  // Detailed rejection message (详细拒单信息)
    bool success() const { return !order_ref.empty(); }
};

struct OrderInfo {
    char instrument_id[24]{};
    char exchange_id[8]{};
    char account_id[16]{};
    char order_ref[16]{};
    char strategy_id[32]{};
    char order_sys_id[24]{};
    Direction direction = Direction::Buy;
    Offset offset = Offset::Open;
    double price = 0.0;
    int total_volume = 0;
    int traded_volume = 0;
    OrderStatus status = OrderStatus::Pending;
    int front_id = 0;
    int session_id = 0;
    char status_msg[80]{};
    char insert_time[12]{};
};

struct TradeInfo {
    char instrument_id[24]{};
    char exchange_id[8]{};
    char account_id[16]{};
    char trade_id[24]{};
    char order_ref[16]{};
    char strategy_id[32]{};
    Direction direction = Direction::Buy;
    Offset offset = Offset::Open;
    double price = 0.0;
    int volume = 0;
    char trade_time[12]{};
};

struct PositionInfo {
    char instrument_id[24]{};
    char account_id[16]{};
    Direction direction = Direction::Buy;
    int total = 0;
    int today = 0;
    int yesterday = 0;
    double avg_price = 0.0;
    double position_profit = 0.0;
    double use_margin = 0.0;
};

struct AccountInfo {
    char account_id[16]{};
    double balance = 0.0;
    double available = 0.0;
    double margin = 0.0;
    double commission = 0.0;
    double close_profit = 0.0;
    double position_profit = 0.0;
    double frozen_margin = 0.0;
    double frozen_commission = 0.0;
};

struct CancelRejectInfo {
    char account_id[16]{};
    char order_ref[16]{};
    char reason[128]{};
};

inline const char* get_exchange_id(const char* instrument) {
    if (!instrument || instrument[0] == '\0') {
        return "";
    }

    const char c0 = instrument[0];
    const char c1 = instrument[1];
    if ((c0 == 'I' && (c1 == 'F' || c1 == 'H' || c1 == 'C' || c1 == 'M' || c1 == 'O')) ||
        (c0 == 'M' && c1 == 'O') ||
        (c0 == 'H' && c1 == 'O') ||
        (c0 == 'T' && (c1 == 'F' || c1 == 'S' || c1 == 'L' || (c1 >= '0' && c1 <= '9')))) {
        return "CFFEX";
    }

    if (((c0 == 's' || c0 == 'S') && (c1 == 'i' || c1 == 'I')) ||
        ((c0 == 'l' || c0 == 'L') && (c1 == 'c' || c1 == 'C'))) {
        return "GFEX";
    }

    if ((c0 == 'r' && c1 == 'b') || (c0 == 'h' && c1 == 'c') ||
        (c0 == 'c' && c1 == 'u') || (c0 == 'a' && c1 == 'l') ||
        (c0 == 'z' && c1 == 'n') || (c0 == 'p' && c1 == 'b') ||
        (c0 == 'n' && c1 == 'i') || (c0 == 's' && c1 == 'n') ||
        (c0 == 'a' && c1 == 'u') || (c0 == 'a' && c1 == 'g') ||
        (c0 == 'b' && c1 == 'u') || (c0 == 'r' && c1 == 'u') ||
        (c0 == 'w' && c1 == 'r') || (c0 == 'f' && c1 == 'u') ||
        (c0 == 's' && c1 == 's') || (c0 == 's' && c1 == 'p') ||
        (c0 == 'a' && c1 == 'o') || (c0 == 'b' && c1 == 'r')) {
        return "SHFE";
    }

    if ((c0 == 's' && c1 == 'c') || (c0 == 'n' && c1 == 'r') ||
        (c0 == 'l' && c1 == 'u') || (c0 == 'b' && c1 == 'c') ||
        (c0 == 'e' && c1 == 'c')) {
        return "INE";
    }

    if (c0 >= 'A' && c0 <= 'Z') {
        return "CZCE";
    }

    return "DCE";
}

inline bool need_close_today_flag(const char* exchange_id) {
    return str_equal(exchange_id, "SHFE") || str_equal(exchange_id, "INE");
}

// ---- RMS Risk Control Mode (RMS 风控模式) ----
enum class RiskMode {
    Normal,       // Normal trading (正常交易)
    Warning,      // Warning (allow trading, highlight in UI) (预警：允许交易，前端高亮提示)
    NoOpen,       // No opening positions (close/cancel allowed) (禁止开仓：平仓/撤单允许)
    ReduceOnly,   // Only reduce positions (backend judges risk reduction) (只允许减仓：后端判断是否减风险)
    Liquidating,  // Force liquidation in progress (close only, auto-triggered) (强制平仓中：只允许平仓，自动触发)
    Halted        // Fully stopped (reject all trading) (全面停止：拒绝一切交易操作)
};

inline const char* to_string(RiskMode mode) {
    switch (mode) {
        case RiskMode::Normal:     return "normal";
        case RiskMode::Warning:    return "warning";
        case RiskMode::NoOpen:     return "no_open";
        case RiskMode::ReduceOnly: return "reduce_only";
        case RiskMode::Liquidating:return "liquidating";
        case RiskMode::Halted:     return "halted";
    }
    return "normal";
}

enum class RiskErrorCode {
    None,
    RISK_NO_OPEN,          // No opening positions (禁止开仓)
    RISK_REDUCE_ONLY,      // Reduce only (只允许减仓)
    RISK_LIQUIDATING,      // Liquidating (强制平仓中)
    RISK_HALTED,           // Fully halted (全面停止)
    MAX_POSITION,          // Net position limit exceeded (净持仓超限)
    MAX_ORDER_SIZE,        // Single order size limit exceeded (单笔手数超限)
    DAILY_LOSS_LIMIT,      // Daily loss limit exceeded (日亏损限制)
    ORDER_RATE_LIMIT,      // Order rate limit exceeded (报单频率限制)
    INVALID_PRICE          // Invalid order price (委托价格不合法)
};

inline const char* to_string(RiskErrorCode code) {
    switch (code) {
        case RiskErrorCode::None:            return "";
        case RiskErrorCode::RISK_NO_OPEN:    return "RISK_NO_OPEN";
        case RiskErrorCode::RISK_REDUCE_ONLY:return "RISK_REDUCE_ONLY";
        case RiskErrorCode::RISK_LIQUIDATING:return "RISK_LIQUIDATING";
        case RiskErrorCode::RISK_HALTED:     return "RISK_HALTED";
        case RiskErrorCode::MAX_POSITION:    return "MAX_POSITION";
        case RiskErrorCode::MAX_ORDER_SIZE:  return "MAX_ORDER_SIZE";
        case RiskErrorCode::DAILY_LOSS_LIMIT:return "DAILY_LOSS_LIMIT";
        case RiskErrorCode::ORDER_RATE_LIMIT:return "ORDER_RATE_LIMIT";
        case RiskErrorCode::INVALID_PRICE:   return "INVALID_PRICE";
    }
    return "";
}

struct RiskEvent {
    int64_t timestamp_ms = 0;
    RiskMode mode = RiskMode::Normal;
    RiskErrorCode error_code = RiskErrorCode::None;
    char instrument_id[24]{};
    char account_id[16]{};
    char message[128]{};
};

enum class ConditionType { StopLoss, TakeProfit, TrailingStop };

enum class CondOrderStatus : int { Pending = 0, Triggered = 1, Cancelled = 2, Expired = 3, Rejected = 4 };

inline const char* cond_status_str(CondOrderStatus s) {
    switch (s) {
        case CondOrderStatus::Pending:   return "pending";
        case CondOrderStatus::Triggered: return "triggered";
        case CondOrderStatus::Cancelled: return "cancelled";
        case CondOrderStatus::Expired:   return "expired";
        case CondOrderStatus::Rejected:  return "rejected";
    }
    return "unknown";
}

struct ConditionalOrder {
    uint32_t id = 0;
    char instrument_id[24]{};
    char account_id[16]{};
    char strategy_id[32]{};
    ConditionType type = ConditionType::StopLoss;
    Direction direction = Direction::Buy;
    Offset offset = Offset::Close;
    double trigger_price = 0.0;
    double order_price = 0.0;
    double trail_offset = 0.0;
    int volume = 0;
    bool active = true;
    double extreme_price = 0.0;
    uint32_t group_id = 0;
    char idempotency_key[64]{};
    CondOrderStatus status = CondOrderStatus::Pending;
    int64_t created_at_ms = 0;
    int64_t triggered_at_ms = 0;
    int64_t cancelled_at_ms = 0;
    int64_t expire_at_ms = 0;
    char reject_reason[64]{};
};

static_assert(std::is_trivially_copyable_v<PriceLevel>, "PriceLevel must be trivially copyable");
static_assert(std::is_trivially_copyable_v<TickData>, "TickData must be trivially copyable");
static_assert(std::is_trivially_copyable_v<OrderInfo>, "OrderInfo must be trivially copyable");
static_assert(std::is_trivially_copyable_v<TradeInfo>, "TradeInfo must be trivially copyable");
static_assert(std::is_trivially_copyable_v<ConditionalOrder>, "ConditionalOrder must be trivially copyable");

} // namespace hft


