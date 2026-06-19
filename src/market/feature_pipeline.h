#pragma once
// ============================================
// feature_pipeline.h - AI/ML feature extraction pipeline
// Built-in indicators: SMA, EMA, RSI, ATR, VWAP, bid_ask_imbalance, order_flow_toxicity
// Per-instrument ring buffers for price/volume history.
// ============================================

#include "common/config.h"
#include "common/types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

class FeatureRingBuffer {
public:
    explicit FeatureRingBuffer(size_t capacity = 10000)
        : cap_(capacity), data_(capacity, 0.0) {}

    void push(double val) {
        data_[head_ % cap_] = val;
        ++head_;
        if (count_ < cap_) ++count_;
    }

    double operator[](size_t ago) const {
        if (ago >= count_) return 0.0;
        return data_[(head_ - 1 - ago) % cap_];
    }

    size_t count() const { return count_; }
    size_t capacity() const { return cap_; }

    double sum_last(size_t n) const {
        n = std::min(n, count_);
        double s = 0.0;
        for (size_t i = 0; i < n; ++i) s += (*this)[i];
        return s;
    }

private:
    size_t cap_;
    std::vector<double> data_;
    size_t head_ = 0;
    size_t count_ = 0;
};

struct InstrumentFeatures {
    FeatureRingBuffer prices;
    FeatureRingBuffer volumes;
    FeatureRingBuffer highs;
    FeatureRingBuffer lows;
    FeatureRingBuffer closes;
    FeatureRingBuffer turnovers;

    double ema_value = 0.0;
    bool ema_initialized = false;

    double last_bid_ask_imbalance = 0.0;
    double vwap = 0.0;
    double cumulative_turnover = 0.0;
    double cumulative_volume = 0.0;

    size_t tick_count = 0;

    explicit InstrumentFeatures(size_t history_size = 10000)
        : prices(history_size), volumes(history_size),
          highs(history_size), lows(history_size),
          closes(history_size), turnovers(history_size) {}
};

class FeaturePipeline {
public:
    void init(const Config& config) {
        enabled_ = config.get_int("Features", "Enable", 0) != 0;
        history_size_ = static_cast<size_t>(config.get_int("Features", "HistorySize", 10000));
        sma_period_ = static_cast<size_t>(config.get_int("Features", "SmaPeriod", 20));
        ema_period_ = static_cast<size_t>(config.get_int("Features", "EmaPeriod", 20));
        rsi_period_ = static_cast<size_t>(config.get_int("Features", "RsiPeriod", 14));
        atr_period_ = static_cast<size_t>(config.get_int("Features", "AtrPeriod", 14));
    }

    bool is_enabled() const { return enabled_; }

    void on_tick(const TickData& tick) {
        if (!enabled_) return;

        std::string inst(tick.instrument_id);
        auto it = features_.find(inst);
        if (it == features_.end()) {
            it = features_.emplace(inst, InstrumentFeatures(history_size_)).first;
        }

        auto& f = it->second;
        double price = tick.last_price;
        double vol = static_cast<double>(tick.volume);

        f.prices.push(price);
        f.volumes.push(vol);
        f.highs.push(tick.highest_price);
        f.lows.push(tick.lowest_price);
        f.closes.push(price);
        f.turnovers.push(tick.turnover);

        // EMA
        if (!f.ema_initialized && f.prices.count() >= ema_period_) {
            f.ema_value = f.prices.sum_last(ema_period_) / static_cast<double>(ema_period_);
            f.ema_initialized = true;
        } else if (f.ema_initialized) {
            double k = 2.0 / (static_cast<double>(ema_period_) + 1.0);
            f.ema_value = price * k + f.ema_value * (1.0 - k);
        }

        // Bid-ask imbalance
        double bid_vol = static_cast<double>(tick.bid[0].volume);
        double ask_vol = static_cast<double>(tick.ask[0].volume);
        double total = bid_vol + ask_vol;
        f.last_bid_ask_imbalance = (total > 0.0) ? (bid_vol - ask_vol) / total : 0.0;

        // VWAP
        f.cumulative_turnover += tick.turnover;
        f.cumulative_volume += vol;
        f.vwap = (f.cumulative_volume > 0.0) ? f.cumulative_turnover / f.cumulative_volume : price;

        ++f.tick_count;
    }

    double get_feature(const std::string& instrument, const std::string& name) const {
        auto it = features_.find(instrument);
        if (it == features_.end()) return 0.0;
        const auto& f = it->second;

        if (name == "sma") return compute_sma(f, sma_period_);
        if (name == "ema") return f.ema_value;
        if (name == "rsi") return compute_rsi(f, rsi_period_);
        if (name == "atr") return compute_atr(f, atr_period_);
        if (name == "vwap") return f.vwap;
        if (name == "bid_ask_imbalance") return f.last_bid_ask_imbalance;
        if (name == "order_flow_toxicity") return compute_toxicity(f);
        if (name == "last_price") return f.prices.count() > 0 ? f.prices[0] : 0.0;
        if (name == "tick_count") return static_cast<double>(f.tick_count);
        return 0.0;
    }

    void reset() { features_.clear(); }

private:
    static double compute_sma(const InstrumentFeatures& f, size_t period) {
        if (f.prices.count() < period) return 0.0;
        return f.prices.sum_last(period) / static_cast<double>(period);
    }

    static double compute_rsi(const InstrumentFeatures& f, size_t period) {
        if (f.prices.count() < period + 1) return 50.0;
        double gain_sum = 0.0, loss_sum = 0.0;
        for (size_t i = 0; i < period; ++i) {
            double diff = f.prices[i] - f.prices[i + 1];
            if (diff > 0) gain_sum += diff;
            else loss_sum -= diff;
        }
        double avg_gain = gain_sum / static_cast<double>(period);
        double avg_loss = loss_sum / static_cast<double>(period);
        if (avg_loss < 1e-12) return 100.0;
        double rs = avg_gain / avg_loss;
        return 100.0 - 100.0 / (1.0 + rs);
    }

    static double compute_atr(const InstrumentFeatures& f, size_t period) {
        if (f.highs.count() < period) return 0.0;
        double sum = 0.0;
        for (size_t i = 0; i < period; ++i) {
            double h = f.highs[i];
            double l = f.lows[i];
            double tr = h - l;
            if (i + 1 < f.closes.count()) {
                double prev_c = f.closes[i + 1];
                tr = std::max(tr, std::max(std::abs(h - prev_c), std::abs(l - prev_c)));
            }
            sum += tr;
        }
        return sum / static_cast<double>(period);
    }

    static double compute_toxicity(const InstrumentFeatures& f) {
        if (f.prices.count() < 2) return 0.0;
        double price_change = f.prices[0] - f.prices[1];
        double vol = f.volumes[0];
        if (vol < 1.0) return 0.0;
        return std::abs(price_change) / std::sqrt(vol);
    }

    bool enabled_ = false;
    size_t history_size_ = 10000;
    size_t sma_period_ = 20;
    size_t ema_period_ = 20;
    size_t rsi_period_ = 14;
    size_t atr_period_ = 14;
    std::unordered_map<std::string, InstrumentFeatures> features_;
};

} // namespace hft
