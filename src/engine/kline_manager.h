#pragma once
// ============================================
// kline_manager.h - K-line data management (K 线数据管理)
// Aggregates tick data into K-line bars by period (1m / 5m / 15m / 1d).
// (将行情 tick 按周期聚合成 K 线柱)
// ============================================

#include "common/types.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace hft {

struct KlineBar {
    int64_t timestamp_ms = 0;
    std::string time;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    int volume = 0;
    double turnover = 0.0;
};

struct KlineCatalogItem {
    std::string instrument;
    std::string period;
    size_t bar_count = 0;
    int64_t first_timestamp_ms = 0;
    int64_t last_timestamp_ms = 0;
};

struct CompletedBar {
    std::string instrument;
    std::string period;
    KlineBar bar;
};

class KlineManager {
public:
    void set_store_path(const std::filesystem::path& path) { store_path_ = path; }
    const std::filesystem::path& store_path() const { return store_path_; }

    std::vector<CompletedBar> update_from_tick(const TickData& tick);

    void maybe_persist(long long steady_us_now);

    std::vector<KlineBar> get_bars(const std::string& instrument,
                                   const std::string& period,
                                   size_t limit = 200) const;
    std::vector<std::string> get_periods(const std::string& instrument) const;
    std::vector<KlineCatalogItem> get_catalog(const std::string& instrument = "",
                                              const std::string& period = "") const;
    bool import_csv(const std::string& instrument,
                    const std::string& period,
                    const std::string& csv_path,
                    bool replace_existing,
                    size_t* imported_count = nullptr,
                    std::string* error = nullptr);

    bool load_store();
    void save_store() const;
    void restore_legacy_bars(std::vector<std::tuple<std::string, std::string, KlineBar>>& items);

private:
    struct InstrumentKlineState {
        int last_volume = -1;
        double last_turnover = -1.0;
        std::string last_trading_day;
        std::map<std::string, std::deque<KlineBar>> bars_by_period;
    };

    std::filesystem::path store_path_ = "runtime_kline.kdb";
    mutable std::mutex mtx_;
    std::map<std::string, InstrumentKlineState> cache_;
    std::atomic<long long> next_persist_steady_ms_{0};
};

} // namespace hft
