#pragma once
// ============================================
// tick_data_manager.h - Tick data storage and retrieval (行情数据存储与查询)
// Thread-safe tick cache with shared_mutex for concurrent read/write access.
// (线程安全的行情缓存, 使用 shared_mutex 实现并发读写)
// ============================================

#include "common/types.h"

#include <atomic>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

class TickDataManager {
public:
    void update(const TickData& tick);

    TickData get_last_tick(const char* instrument) const;
    std::unordered_map<std::string, TickData> get_all_ticks() const;
    std::unordered_map<std::string, TickData> get_ticks_filtered(
        const std::vector<std::string>& instruments = {},
        size_t limit = 0) const;
    std::vector<TickData> get_ticks_changed_since(
        long long since_update_seq,
        size_t limit,
        long long* latest_update_seq = nullptr) const;
    std::vector<TickData> get_ticks_for(const std::vector<std::string>& instruments) const;

private:
    struct Entry {
        TickData tick{};
        long long seq = 0;
    };
    // Single map stores both tick and seq, saving one hash lookup and one InstrumentKey construction.
    // Uses shared_mutex: hot-path writer (consumer thread) uses unique_lock, REST/UI readers use
    // shared_lock. Concurrent reads and writes no longer block each other.
    // (单 map 同时存 tick 和 seq, 省一次哈希查找与一次 InstrumentKey 构造。
    //  用 shared_mutex: 热路径写者 consumer 线程用 unique_lock, REST/UI 读者用
    //  shared_lock。读写并发不再相互阻塞。)
    mutable std::shared_mutex mtx_;
    std::unordered_map<InstrumentKey, Entry, InstrumentKeyHash> entries_;
    std::atomic<long long> seq_counter_{0};
};

} // namespace hft
