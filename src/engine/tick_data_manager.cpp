// ============================================
// tick_data_manager.cpp - Tick data storage and retrieval implementation (行情数据存储与查询实现)
// ============================================

#include "engine/tick_data_manager.h"

#include <algorithm>

namespace hft {

void TickDataManager::update(const TickData& tick) {
    const long long seq = seq_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::unique_lock<std::shared_mutex> lock(mtx_);
    auto& entry = entries_[InstrumentKey(tick.instrument_id)];
    entry.tick = tick;
    entry.seq = seq;
}

TickData TickDataManager::get_last_tick(const char* instrument) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    auto it = entries_.find(InstrumentKey(instrument));
    if (it != entries_.end()) return it->second.tick;
    return TickData{};
}

std::unordered_map<std::string, TickData> TickDataManager::get_all_ticks() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    std::unordered_map<std::string, TickData> result;
    result.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        result.emplace(std::string(key.data), entry.tick);
    }
    return result;
}

std::unordered_map<std::string, TickData> TickDataManager::get_ticks_filtered(
    const std::vector<std::string>& instruments,
    size_t limit) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    std::unordered_map<std::string, TickData> result;
    if (!instruments.empty()) {
        for (const auto& inst : instruments) {
            auto it = entries_.find(InstrumentKey(inst.c_str()));
            if (it == entries_.end()) continue;
            result.emplace(inst, it->second.tick);
            if (limit > 0 && result.size() >= limit) break;
        }
        return result;
    }
    for (const auto& [key, entry] : entries_) {
        result.emplace(std::string(key.data), entry.tick);
        if (limit > 0 && result.size() >= limit) break;
    }
    return result;
}

std::vector<TickData> TickDataManager::get_ticks_changed_since(
    long long since_update_seq,
    size_t limit,
    long long* latest_update_seq) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    std::vector<TickData> result;
    long long emitted_latest_seq = since_update_seq;
    if (latest_update_seq) *latest_update_seq = emitted_latest_seq;
    if (limit == 0) limit = 2000;

    std::vector<std::pair<long long, TickData>> changed;
    changed.reserve((std::min)(limit, entries_.size()));
    for (const auto& item : entries_) {
        const long long seq = item.second.seq;
        if (seq <= since_update_seq) continue;
        changed.emplace_back(seq, item.second.tick);
    }
    std::sort(changed.begin(), changed.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    result.reserve((std::min)(limit, changed.size()));
    for (const auto& item : changed) {
        result.push_back(item.second);
        emitted_latest_seq = item.first;
        if (result.size() >= limit) break;
    }
    if (latest_update_seq) *latest_update_seq = emitted_latest_seq;
    return result;
}

std::vector<TickData> TickDataManager::get_ticks_for(const std::vector<std::string>& instruments) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    std::vector<TickData> result;
    for (const auto& inst : instruments) {
        auto it = entries_.find(InstrumentKey(inst.c_str()));
        if (it != entries_.end()) {
            result.push_back(it->second.tick);
        }
    }
    return result;
}

} // namespace hft
