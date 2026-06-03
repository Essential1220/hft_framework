// ============================================
// simple_strategy.cpp - 一个基于 C++ 的原生简单动量策略实现示例
//
// 策略逻辑：
//   监听指定合约的 Tick 数据，维护最近的 N+1 个最新价。
//   - 如果价格连续 N 次上涨，并且当前没有多头持仓，则以卖一价（对手价）发出买入开多信号（或买入平空）。
//   - 如果价格连续 N 次下跌，并且当前没有空头持仓，则以买一价（对手价）发出卖出开空信号（或卖出平多）。
//   - 信号触发后进入一段冷却期（Cooldown），期间忽略所有行情，防止在剧烈波动时重复发单。
//
// 设计意图：
//   作为 C++ 策略的编写范例，展示了如何继承 StrategyBase，如何维护内部状态（仓位、价格队列），
//   以及如何响应引擎的生命周期回调（on_init, on_tick, on_trade, on_reconnect）。
// ============================================

#include "strategy/simple_strategy.h"

#include "common/logger.h"

namespace hft {

SimpleStrategy::SimpleStrategy(const char* instrument, int order_size,
                               int momentum_ticks, int cooldown_seconds)
    : order_size_(order_size)           // 每次发单的固定手数
    , momentum_ticks_(momentum_ticks)   // 触发信号所需的连续上涨/下跌的 Tick 数量 (N)
    , cooldown_seconds_(cooldown_seconds) // 信号触发后的冷却时间（秒）
{
    // 绑定策略监听和交易的目标合约
    safe_copy(instrument_, instrument, sizeof(instrument_));
}

// 策略生命周期：初始化
// 在引擎启动并准备好后被调用。
// 必须在此时从引擎底层同步一次真实的净持仓，避免策略内部的逻辑状态与账户实际状态发生漂移。
void SimpleStrategy::on_init() {
    sync_position_from_engine();
    LOG_INFO("策略初始化完成: 合约=" + std::string(instrument_) +
             " 下单手数=" + std::to_string(order_size_) +
             " 动量Tick数=" + std::to_string(momentum_ticks_) +
             " 冷却秒数=" + std::to_string(cooldown_seconds_) +
             " 初始净仓=" + std::to_string(position_));
}

// 策略生命周期：行情驱动
// 每次收到市场广播的最新 Tick 数据时触发，是高频策略的核心计算入口。
void SimpleStrategy::on_tick(const TickData& tick) {
    // 过滤掉非目标合约的行情
    if (!str_equal(tick.instrument_id, instrument_)) return;
    // 过滤掉异常的极端价格（如开盘前竞价阶段可能推送 0 或超大值）
    if (tick.last_price <= 0 || tick.last_price > 1e8) return;

    // 维护固定长度的滑动价格窗口
    price_history_.push_back(tick.last_price);
    while (static_cast<int>(price_history_.size()) > momentum_ticks_ + 1) {
        price_history_.pop_front();
    }

    // 如果数据点还不够，继续收集，不产生信号
    if (static_cast<int>(price_history_.size()) < momentum_ticks_ + 1) return;
    
    // 如果策略正在冷却期，跳过信号判断
    if (is_cooling_down()) return;

    // 动量逻辑判断：遍历滑动窗口，检查是否完全单调递增或单调递减
    bool all_up = true;
    bool all_down = true;
    for (int i = 1; i < static_cast<int>(price_history_.size()); ++i) {
        if (price_history_[i] <= price_history_[i - 1]) all_up = false;
        if (price_history_[i] >= price_history_[i - 1]) all_down = false;
    }
    
    // 如果既不是连续上涨也不是连续下跌，说明处于震荡，直接返回
    if (!all_up && !all_down) return;

    // 盘口一档价格有效性校验
    // 必须确保盘口有真实的挂单，避免使用涨跌停板的无效价格（如涨停时卖一量为0价为0）作为报单价格
    if (!is_valid_bid_ask(tick)) {
        LOG_WARN("策略跳过信号，原因：买一/卖一价格无效。");
        return;
    }

    // 准备构造报单请求
    OrderRequest req{};
    safe_copy(req.instrument_id, instrument_, sizeof(req.instrument_id));
    safe_copy(req.exchange_id, get_exchange_id(instrument_), sizeof(req.exchange_id));
    req.volume = order_size_;

    // ---- 连续上涨：做多逻辑 ----
    if (all_up) {
        if (position_ < 0) {
            // 如果当前持有空头（净仓 < 0），则先平掉空头（买入平仓）
            req.direction = Direction::Buy;
            req.offset = Offset::Close;
            req.price = tick.ask[0].price; // 使用对手价（卖一价）确保成交
            LOG_INFO("策略信号：买入平空 " + std::string(instrument_) +
                     " 价格=" + std::to_string(req.price));
            send_order(req);
        } else if (position_ == 0) {
            // 如果当前空仓，则建立新的多头（买入开仓）
            req.direction = Direction::Buy;
            req.offset = Offset::Open;
            req.price = tick.ask[0].price;
            LOG_INFO("策略信号：买入开多 " + std::string(instrument_) +
                     " 价格=" + std::to_string(req.price));
            send_order(req);
        }
    // ---- 连续下跌：做空逻辑 ----
    } else if (all_down) {
        if (position_ > 0) {
            // 如果当前持有多头（净仓 > 0），则先平掉多头（卖出平仓）
            req.direction = Direction::Sell;
            req.offset = Offset::Close;
            req.price = tick.bid[0].price; // 使用对手价（买一价）确保成交
            LOG_INFO("策略信号：卖出平多 " + std::string(instrument_) +
                     " 价格=" + std::to_string(req.price));
            send_order(req);
        } else if (position_ == 0) {
            // 如果当前空仓，则建立新的空头（卖出开仓）
            req.direction = Direction::Sell;
            req.offset = Offset::Open;
            req.price = tick.bid[0].price;
            LOG_INFO("策略信号：卖出开空 " + std::string(instrument_) +
                     " 价格=" + std::to_string(req.price));
            send_order(req);
        }
    }

    // 触发信号后，记录当前时间，使策略进入冷却期
    last_signal_time_ = std::chrono::steady_clock::now();
    in_cooldown_ = true;
}

// 策略生命周期：委托回报
// 接收订单状态的变化（如提交成功、部分成交、已撤销、废单等）
void SimpleStrategy::on_order(const OrderInfo& order) {
    if (!str_equal(order.instrument_id, instrument_)) return;
    LOG_INFO("策略收到委托回报: ref=" + std::string(order.order_ref) +
             " 状态=" + std::to_string(static_cast<int>(order.status)) +
             " 信息=" + std::string(order.status_msg));
}

// 策略生命周期：成交回报
// 用于在策略内部实时动态更新仓位。
//
// 仓位更新的数学本质：
//   由于 `position_` 代表净持仓（多头 - 空头）：
//   - 无论开仓还是平仓，只要是“买入(Buy)”动作，都会导致净持仓增加。
//     (买入开仓：多头增加，净仓+；买入平仓：空头减少，净仓+)
//   - 无论开仓还是平仓，只要是“卖出(Sell)”动作，都会导致净持仓减少。
//     (卖出开仓：空头增加，净仓-；卖出平仓：多头减少，净仓-)
// 这种基于“净持仓”的简化计算，免去了分别跟踪多头和空头数组的麻烦。
void SimpleStrategy::on_trade(const TradeInfo& trade) {
    if (!str_equal(trade.instrument_id, instrument_)) return;

    const int delta = trade.volume;
    position_ += (trade.direction == Direction::Buy) ? delta : -delta;

    LOG_INFO("策略成交回报: 合约=" + std::string(trade.instrument_id) +
             " 方向=" + (trade.direction == Direction::Buy ? "买" : "卖") +
             " 开平=" + std::to_string(static_cast<int>(trade.offset)) +
             " 价格=" + std::to_string(trade.price) +
             " 手数=" + std::to_string(trade.volume) +
             " 当前净仓=" + std::to_string(position_));
}

// 策略生命周期：重连恢复
// 当底层连接断开并重新登录成功后触发。
// 此时可能存在断线期间发生的成交（如挂单被吃），导致内部仓位与实际不符，
// 因此必须重新调用 get_net_position 同步最新底仓。
void SimpleStrategy::on_reconnect() {
    sync_position_from_engine();
    LOG_INFO("策略重连校准完成，当前净仓=" + std::to_string(position_));
}

// 辅助方法：通过调用基类提供的查询接口，获取引擎中记录的准确净持仓
void SimpleStrategy::sync_position_from_engine() {
    position_ = get_net_position(instrument_);
}

// 辅助方法：判断策略是否处于刚发单后的冷却期内
bool SimpleStrategy::is_cooling_down() const {
    if (!in_cooldown_) return false;
    const auto elapsed = std::chrono::steady_clock::now() - last_signal_time_;
    return elapsed < std::chrono::seconds(cooldown_seconds_);
}

// 辅助方法：检查盘口一档（买一/卖一）价格的有效性，防止利用 0 价或脏行情引发废单
// 特殊情况：涨跌停板价为 0 时（部分合约在竞价刚结束首个 tick 可能如此），跳过涨跌停范围校验。
bool SimpleStrategy::is_valid_bid_ask(const TickData& tick) const {
    if (tick.bid[0].price <= 0.0 || tick.ask[0].price <= 0.0) {
        return false;
    }
    // 当交易所下发了有效的涨跌停板价格时，进一步校验盘口报价是否越界
    if (tick.upper_limit > 0.0 && tick.lower_limit > 0.0) {
        if (tick.bid[0].price > tick.upper_limit || tick.bid[0].price < tick.lower_limit) {
            return false;
        }
        if (tick.ask[0].price > tick.upper_limit || tick.ask[0].price < tick.lower_limit) {
            return false;
        }
    }
    return true;
}

} // namespace hft
