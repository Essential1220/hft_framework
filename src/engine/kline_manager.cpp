// ============================================
// kline_manager.cpp - K-line data management, extracted from TradingEngine (K 线数据管理, 从 TradingEngine 中提取)
// Aggregates tick data into K-line bars by period (1m/5m/15m/1d), supports CSV import/export,
// and binary store persistence.
// (将行情 tick 按周期聚合成 K 线柱 1m/5m/15m/1d, 支持 CSV 导入导出和二进制存储持久化)

#include "engine/kline_manager.h"

#include "common/binary_io.h"
#include "common/logger.h"
#include "common/string_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace hft {

namespace {

using namespace hft::binary_io;

constexpr size_t kPersistedKlineBarsPerPeriod = 400;
constexpr char kKlineStoreMagic[] = "HFTKDB1";

std::tm local_time_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    return tm_buf;
}

int64_t local_date_time_to_epoch_ms(const std::string& day, const std::string& time, int millisec) {
    std::tm tm_buf{};
    try {
        if (day.size() == 8) {
            tm_buf.tm_year = std::stoi(day.substr(0, 4)) - 1900;
            tm_buf.tm_mon = std::stoi(day.substr(4, 2)) - 1;
            tm_buf.tm_mday = std::stoi(day.substr(6, 2));
        } else {
            const auto now = local_time_now();
            tm_buf.tm_year = now.tm_year;
            tm_buf.tm_mon = now.tm_mon;
            tm_buf.tm_mday = now.tm_mday;
        }

        if (time.size() >= 8) {
            tm_buf.tm_hour = std::stoi(time.substr(0, 2));
            tm_buf.tm_min = std::stoi(time.substr(3, 2));
            tm_buf.tm_sec = std::stoi(time.substr(6, 2));
        }
    } catch (...) {
        return 0;
    }

    tm_buf.tm_isdst = -1;
    const std::time_t seconds = std::mktime(&tm_buf);
    if (seconds < 0) {
        return 0;
    }
    return static_cast<int64_t>(seconds) * 1000 + std::max(millisec, 0);
}

int64_t period_bucket_ms(const std::string& period) {
    if (period == "5m") return 5LL * 60 * 1000;
    if (period == "15m") return 15LL * 60 * 1000;
    if (period == "1d") return 24LL * 60 * 60 * 1000;
    return 60LL * 1000;
}

int64_t bucket_start_ms(int64_t timestamp_ms, const std::string& period) {
    const int64_t bucket = period_bucket_ms(period);
    if (bucket <= 0) {
        return timestamp_ms;
    }
    return timestamp_ms - (timestamp_ms % bucket);
}

int64_t trading_day_to_bucket_ms(const char* trading_day) {
    if (!trading_day || trading_day[0] == '\0') return 0;
    try {
        const std::string td(trading_day);
        if (td.size() != 8) return 0;
        std::tm tm_buf{};
        tm_buf.tm_year = std::stoi(td.substr(0, 4)) - 1900;
        tm_buf.tm_mon = std::stoi(td.substr(4, 2)) - 1;
        tm_buf.tm_mday = std::stoi(td.substr(6, 2));
        tm_buf.tm_isdst = -1;
        const std::time_t seconds = std::mktime(&tm_buf);
        if (seconds < 0) return 0;
        return static_cast<int64_t>(seconds) * 1000;
    } catch (...) {
        return 0;
    }
}

std::string format_kline_time(int64_t timestamp_ms, const std::string& period) {
    std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &seconds);
#else
    localtime_r(&seconds, &tm_buf);
#endif

    char buf[16];
    if (period == "1d") {
        std::snprintf(buf, sizeof(buf), "%02d-%02d", tm_buf.tm_mon + 1, tm_buf.tm_mday);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
    }
    return buf;
}

void append_or_update_bar(std::deque<KlineBar>* bars,
                          int64_t bar_timestamp_ms,
                          const std::string& period,
                          double price,
                          int volume_delta,
                          double turnover_delta) {
    if (!bars || price <= 0.0) {
        return;
    }

    if (bars->empty() || bars->back().timestamp_ms != bar_timestamp_ms) {
        KlineBar bar;
        bar.timestamp_ms = bar_timestamp_ms;
        bar.time = format_kline_time(bar_timestamp_ms, period);
        bar.open = price;
        bar.high = price;
        bar.low = price;
        bar.close = price;
        bar.volume = std::max(volume_delta, 0);
        bar.turnover = std::max(turnover_delta, 0.0);
        bars->push_back(std::move(bar));
    } else {
        auto& bar = bars->back();
        bar.high = (std::max)(bar.high, price);
        bar.low = (std::min)(bar.low, price);
        bar.close = price;
        bar.volume += std::max(volume_delta, 0);
        bar.turnover += std::max(turnover_delta, 0.0);
    }

    while (bars->size() > 2000) {
        bars->pop_front();
    }
}

std::string normalize_kline_period(const std::string& period) {
    if (period == "5m" || period == "15m" || period == "1d") {
        return period;
    }
    return "1m";
}

std::vector<std::string> collect_import_periods(const std::string& base_period) {
    if (base_period == "1m") {
        return {"1m", "5m", "15m", "1d"};
    }
    if (base_period == "5m") {
        return {"5m", "15m", "1d"};
    }
    if (base_period == "15m") {
        return {"15m", "1d"};
    }
    return {"1d"};
}

std::vector<std::string> sort_kline_periods(const std::vector<std::string>& periods) {
    std::vector<std::string> ordered;
    for (const std::string candidate : {"1m", "5m", "15m", "1d"}) {
        if (std::find(periods.begin(), periods.end(), candidate) != periods.end()) {
            ordered.push_back(candidate);
        }
    }
    return ordered;
}

bool split_csv_record(const std::string& line, std::vector<std::string>* fields) {
    if (!fields) {
        return false;
    }

    fields->clear();
    std::string current;
    bool in_quotes = false;

    for (size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (ch == '"') {
            if (in_quotes && index + 1 < line.size() && line[index + 1] == '"') {
                current.push_back('"');
                ++index;
            } else {
                in_quotes = !in_quotes;
            }
            continue;
        }

        if (ch == ',' && !in_quotes) {
            fields->push_back(trim_copy(current));
            current.clear();
            continue;
        }

        current.push_back(ch);
    }

    if (in_quotes) {
        return false;
    }

    fields->push_back(trim_copy(current));
    return true;
}

std::string normalize_csv_header(std::string text) {
    text = lower_copy(trim_copy(std::move(text)));
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

bool parse_double_value(const std::string& text, double* out) {
    if (!out) {
        return false;
    }

    try {
        const std::string trimmed = trim_copy(text);
        if (trimmed.empty()) {
            return false;
        }
        *out = std::stod(trimmed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_integer_value(const std::string& text, int* out) {
    double value = 0.0;
    if (!out || !parse_double_value(text, &value)) {
        return false;
    }

    *out = static_cast<int>(value);
    return true;
}

bool parse_epoch_value(const std::string& text, int64_t* out_timestamp_ms) {
    if (!out_timestamp_ms) {
        return false;
    }

    const std::string trimmed = trim_copy(text);
    if (trimmed.empty() ||
        !std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        return false;
    }

    try {
        const int64_t value = std::stoll(trimmed);
        if (trimmed.size() >= 13) {
            *out_timestamp_ms = value;
        } else if (trimmed.size() == 10) {
            *out_timestamp_ms = value * 1000;
        } else {
            return false;
        }
        return *out_timestamp_ms > 0;
    } catch (...) {
        return false;
    }
}

bool parse_calendar_timestamp(const std::string& text, int64_t* out_timestamp_ms) {
    if (!out_timestamp_ms) {
        return false;
    }

    const std::string trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return false;
    }

    if (parse_epoch_value(trimmed, out_timestamp_ms)) {
        return true;
    }

    if (std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        try {
            if (trimmed.size() == 8) {
                return local_date_time_to_epoch_ms(trimmed, "00:00:00", 0) > 0
                    ? (*out_timestamp_ms = local_date_time_to_epoch_ms(trimmed, "00:00:00", 0), true)
                    : false;
            }

            if (trimmed.size() >= 12) {
                const std::string date = trimmed.substr(0, 8);
                const std::string hh = trimmed.substr(8, 2);
                const std::string mm = trimmed.substr(10, 2);
                const std::string ss = trimmed.size() >= 14 ? trimmed.substr(12, 2) : "00";
                int millisec = 0;
                if (trimmed.size() > 14) {
                    std::string ms = trimmed.substr(14, (std::min)(size_t{3}, trimmed.size() - 14));
                    while (ms.size() < 3) {
                        ms.push_back('0');
                    }
                    millisec = std::stoi(ms);
                }
                const int64_t value = local_date_time_to_epoch_ms(date, hh + ":" + mm + ":" + ss, millisec);
                if (value > 0) {
                    *out_timestamp_ms = value;
                    return true;
                }
            }
        } catch (...) {
            return false;
        }
    }

    std::vector<std::string> digits;
    std::string current;
    for (char ch : trimmed) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            current.push_back(ch);
        } else if (!current.empty()) {
            digits.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        digits.push_back(current);
    }

    if (digits.size() < 3) {
        return false;
    }

    try {
        const std::string year = digits[0];
        const std::string month = digits[1].size() == 1 ? "0" + digits[1] : digits[1];
        const std::string day = digits[2].size() == 1 ? "0" + digits[2] : digits[2];
        const std::string hour = digits.size() >= 4 ? (digits[3].size() == 1 ? "0" + digits[3] : digits[3]) : "00";
        const std::string minute = digits.size() >= 5 ? (digits[4].size() == 1 ? "0" + digits[4] : digits[4]) : "00";
        const std::string second = digits.size() >= 6 ? (digits[5].size() == 1 ? "0" + digits[5] : digits[5]) : "00";
        int millisec = 0;
        if (digits.size() >= 7) {
            std::string ms = digits[6].substr(0, (std::min)(size_t{3}, digits[6].size()));
            while (ms.size() < 3) {
                ms.push_back('0');
            }
            millisec = std::stoi(ms);
        }

        const int64_t value = local_date_time_to_epoch_ms(year + month + day, hour + ":" + minute + ":" + second, millisec);
        if (value <= 0) {
            return false;
        }

        *out_timestamp_ms = value;
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<KlineBar> aggregate_bars_to_period(const std::vector<KlineBar>& source_bars,
                                               const std::string& period) {
    const std::string normalized_period = normalize_kline_period(period);
    if (source_bars.empty()) {
        return {};
    }

    std::vector<KlineBar> aggregated;
    aggregated.reserve(source_bars.size());

    for (const auto& source : source_bars) {
        if (source.timestamp_ms <= 0) {
            continue;
        }

        const int64_t bucket = bucket_start_ms(source.timestamp_ms, normalized_period);
        if (aggregated.empty() || aggregated.back().timestamp_ms != bucket) {
            KlineBar bar;
            bar.timestamp_ms = bucket;
            bar.time = format_kline_time(bucket, normalized_period);
            bar.open = source.open;
            bar.high = source.high;
            bar.low = source.low;
            bar.close = source.close;
            bar.volume = (std::max)(source.volume, 0);
            bar.turnover = (std::max)(source.turnover, 0.0);
            aggregated.push_back(std::move(bar));
            continue;
        }

        auto& bar = aggregated.back();
        bar.high = (std::max)(bar.high, source.high);
        bar.low = (std::min)(bar.low, source.low);
        bar.close = source.close;
        bar.volume += (std::max)(source.volume, 0);
        bar.turnover += (std::max)(source.turnover, 0.0);
    }

    return aggregated;
}

std::vector<KlineBar> merge_period_bars(const std::deque<KlineBar>* existing,
                                        const std::vector<KlineBar>& imported,
                                        bool replace_existing) {
    std::map<int64_t, KlineBar> merged;
    if (!replace_existing && existing) {
        for (const auto& bar : *existing) {
            merged[bar.timestamp_ms] = bar;
        }
    }

    for (const auto& bar : imported) {
        merged[bar.timestamp_ms] = bar;
    }

    std::vector<KlineBar> result;
    result.reserve(merged.size());
    for (const auto& item : merged) {
        result.push_back(item.second);
    }

    if (result.size() > 2000) {
        result.erase(result.begin(), result.end() - 2000);
    }

    return result;
}

bool read_import_csv_bars(const std::string& csv_path,
                          std::vector<KlineBar>* bars_out,
                          std::string* error_out) {
    if (!bars_out) {
        if (error_out) *error_out = "internal_invalid_output";
        return false;
    }

    std::ifstream ifs(csv_path);
    if (!ifs.is_open()) {
        if (error_out) *error_out = "csv_open_failed";
        return false;
    }

    std::string header_line;
    while (std::getline(ifs, header_line)) {
        if (!trim_copy(header_line).empty()) {
            break;
        }
    }
    if (trim_copy(header_line).empty()) {
        if (error_out) *error_out = "csv_empty";
        return false;
    }

    if (!header_line.empty() && static_cast<unsigned char>(header_line[0]) == 0xEF) {
        if (header_line.size() >= 3 &&
            static_cast<unsigned char>(header_line[1]) == 0xBB &&
            static_cast<unsigned char>(header_line[2]) == 0xBF) {
            header_line.erase(0, 3);
        }
    }

    std::vector<std::string> headers;
    if (!split_csv_record(header_line, &headers) || headers.empty()) {
        if (error_out) *error_out = "csv_header_invalid";
        return false;
    }

    std::map<std::string, size_t> header_index;
    for (size_t index = 0; index < headers.size(); ++index) {
        header_index[normalize_csv_header(headers[index])] = index;
    }

    const auto find_header = [&header_index](std::initializer_list<const char*> candidates) -> int {
        for (const char* candidate : candidates) {
            const auto it = header_index.find(candidate);
            if (it != header_index.end()) {
                return static_cast<int>(it->second);
            }
        }
        return -1;
    };

    const int timestamp_index = find_header({"timestamp", "datetime", "ts"});
    const int date_index = find_header({"date", "tradingday", "day"});
    const int time_index = find_header({"time", "updatetime"});
    const int open_index = find_header({"open", "openprice"});
    const int high_index = find_header({"high", "highprice"});
    const int low_index = find_header({"low", "lowprice"});
    const int close_index = find_header({"close", "closeprice", "lastprice"});
    const int volume_index = find_header({"volume", "vol"});
    const int turnover_index = find_header({"turnover", "amount", "value"});

    if ((timestamp_index < 0 && date_index < 0 && time_index < 0) ||
        open_index < 0 || high_index < 0 || low_index < 0 || close_index < 0) {
        if (error_out) *error_out = "csv_missing_required_columns";
        return false;
    }

    std::vector<KlineBar> rows;
    std::vector<std::string> fields;
    std::string line;
    size_t total_rows = 0;
    size_t parsed_rows = 0;

    while (std::getline(ifs, line)) {
        ++total_rows;
        if (trim_copy(line).empty()) {
            continue;
        }
        if (!split_csv_record(line, &fields)) {
            continue;
        }
        if (fields.size() < headers.size()) {
            fields.resize(headers.size());
        }

        const auto get_field = [&fields](int index) -> std::string {
            return index >= 0 && static_cast<size_t>(index) < fields.size() ? trim_copy(fields[static_cast<size_t>(index)]) : "";
        };

        int64_t timestamp_ms = 0;
        if (timestamp_index >= 0) {
            if (!parse_calendar_timestamp(get_field(timestamp_index), &timestamp_ms)) {
                continue;
            }
        } else if (date_index >= 0) {
            const std::string date_text = get_field(date_index);
            const std::string time_text = get_field(time_index);
            if (!parse_calendar_timestamp(date_text + (time_text.empty() ? "" : " " + time_text), &timestamp_ms)) {
                continue;
            }
        } else {
            if (!parse_calendar_timestamp(get_field(time_index), &timestamp_ms)) {
                continue;
            }
        }

        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        double close = 0.0;
        if (!parse_double_value(get_field(open_index), &open) ||
            !parse_double_value(get_field(high_index), &high) ||
            !parse_double_value(get_field(low_index), &low) ||
            !parse_double_value(get_field(close_index), &close)) {
            continue;
        }

        KlineBar bar;
        bar.timestamp_ms = timestamp_ms;
        bar.open = open;
        bar.high = high;
        bar.low = low;
        bar.close = close;
        bar.time = format_kline_time(timestamp_ms, "1m");
        if (volume_index >= 0) {
            parse_integer_value(get_field(volume_index), &bar.volume);
        }
        if (turnover_index >= 0) {
            parse_double_value(get_field(turnover_index), &bar.turnover);
        }

        rows.push_back(std::move(bar));
        ++parsed_rows;
    }

    if (parsed_rows == 0) {
        if (error_out) *error_out = total_rows == 0 ? "csv_no_data_rows" : "csv_no_usable_rows";
        return false;
    }

    std::sort(rows.begin(), rows.end(), [](const KlineBar& left, const KlineBar& right) {
        if (left.timestamp_ms != right.timestamp_ms) {
            return left.timestamp_ms < right.timestamp_ms;
        }
        return left.close < right.close;
    });

    *bars_out = std::move(rows);
    return true;
}

bool parse_kline_bar_line(const std::string& line,
                          std::string* instrument,
                          std::string* period,
                          KlineBar* bar) {
    if (!instrument || !period || !bar) {
        return false;
    }

    std::istringstream iss(line);
    std::string kind;
    std::string field;
    std::getline(iss, kind, '\t');
    if (kind != "kline_bar") {
        return false;
    }

    if (!std::getline(iss, *instrument, '\t') ||
        !std::getline(iss, *period, '\t') ||
        !std::getline(iss, field, '\t')) {
        return false;
    }

    try {
        bar->timestamp_ms = std::stoll(field);
        if (!std::getline(iss, bar->time, '\t')) return false;
        if (!std::getline(iss, field, '\t')) return false;
        bar->open = std::stod(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->high = std::stod(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->low = std::stod(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->close = std::stod(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->volume = std::stoi(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->turnover = std::stod(field);
    } catch (...) {
        return false;
    }

    return !instrument->empty() && !period->empty();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// KlineManager method implementations (K线管理器方法实现)
// ---------------------------------------------------------------------------

// Aggregate a single tick into all period bars (1m/5m/15m/1d) — hot path called from consumer loop.
// (将单个 tick 聚合到所有周期 K 线中 1m/5m/15m/1d — 热路径, 从消费者循环调用)
std::vector<CompletedBar> KlineManager::update_from_tick(const TickData& tick) {
    std::vector<CompletedBar> completed;
    const std::string instrument = tick.instrument_id;
    if (instrument.empty() || tick.last_price <= 0.0) {
        return completed;
    }

    const char* raw_day = tick.action_day[0] != '\0' ? tick.action_day : tick.trading_day;
    const std::string trade_day(raw_day);
    const std::string update_time = tick.update_time;
    const int64_t timestamp_ms = local_date_time_to_epoch_ms(trade_day, update_time, tick.update_millisec);
    if (timestamp_ms <= 0) {
        return completed;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    auto& state = cache_[instrument];

    if (!state.last_trading_day.empty() && state.last_trading_day != trade_day) {
        state.last_volume = -1;
        state.last_turnover = -1.0;
    }
    state.last_trading_day = trade_day;

    int volume_delta = 0;
    if (state.last_volume >= 0) {
        volume_delta = tick.volume >= state.last_volume ? (tick.volume - state.last_volume) : 0;
    }

    double turnover_delta = 0.0;
    if (state.last_turnover >= 0.0) {
        turnover_delta = tick.turnover >= state.last_turnover ? (tick.turnover - state.last_turnover) : 0.0;
    }

    state.last_volume = tick.volume;
    state.last_turnover = tick.turnover;

    for (const char* period : {"1m", "5m", "15m", "1d"}) {
        const int64_t bar_ts = (std::strcmp(period, "1d") == 0)
            ? trading_day_to_bucket_ms(tick.trading_day)
            : bucket_start_ms(timestamp_ms, period);
        if (bar_ts <= 0) continue;

        auto& bars = state.bars_by_period[period];
        const bool new_bar = bars.empty() || bars.back().timestamp_ms != bar_ts;

        if (new_bar && !bars.empty()) {
            CompletedBar cb;
            cb.instrument = instrument;
            cb.period = period;
            cb.bar = bars.back();
            completed.push_back(std::move(cb));
        }

        append_or_update_bar(
            &bars,
            bar_ts,
            period,
            tick.last_price,
            volume_delta,
            turnover_delta);
    }

    return completed;
}

// Check if it's time to persist K-line data; actual write is triggered by the async save loop.
// (检查是否到了持久化 K 线数据的时间; 实际的写入由异步保存循环触发)
void KlineManager::maybe_persist(long long steady_us_now) {
    const long long now_ms = steady_us_now / 1000;
    const long long next_due = next_persist_steady_ms_.load(std::memory_order_relaxed);
    if (next_due > now_ms) {
        return;
    }

    next_persist_steady_ms_.store(now_ms + 60000, std::memory_order_relaxed);
    // async_save: periodic timer handles this
}

std::vector<KlineBar> KlineManager::get_bars(const std::string& instrument,
                                             const std::string& period,
                                             size_t limit) const {
    if (instrument.empty()) {
        return {};
    }

    const std::string normalized_period = normalize_kline_period(period);

    std::lock_guard<std::mutex> lock(mtx_);
    const auto state_it = cache_.find(instrument);
    if (state_it == cache_.end()) {
        return {};
    }

    const auto bars_it = state_it->second.bars_by_period.find(normalized_period);
    if (bars_it == state_it->second.bars_by_period.end() || bars_it->second.empty()) {
        return {};
    }

    const auto& bars = bars_it->second;
    const size_t count = (std::min)(bars.size(), limit == 0 ? size_t{200} : limit);
    return std::vector<KlineBar>(bars.end() - count, bars.end());
}

std::vector<std::string> KlineManager::get_periods(const std::string& instrument) const {
    if (instrument.empty()) {
        return {};
    }

    std::vector<std::string> periods;
    std::lock_guard<std::mutex> lock(mtx_);
    const auto state_it = cache_.find(instrument);
    if (state_it == cache_.end()) {
        return {};
    }

    for (const auto& [period, bars] : state_it->second.bars_by_period) {
        if (!bars.empty()) {
            periods.push_back(period);
        }
    }

    return sort_kline_periods(periods);
}

std::vector<KlineCatalogItem> KlineManager::get_catalog(const std::string& instrument,
                                                        const std::string& period) const {
    const std::string instrument_filter = trim_copy(instrument);
    const std::string period_filter = trim_copy(period);
    std::vector<KlineCatalogItem> items;

    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [cached_instrument, state] : cache_) {
        if (!instrument_filter.empty() && cached_instrument != instrument_filter) {
            continue;
        }
        for (const auto& [cached_period, bars] : state.bars_by_period) {
            if (!period_filter.empty() && cached_period != period_filter) {
                continue;
            }
            if (bars.empty()) {
                continue;
            }

            KlineCatalogItem item;
            item.instrument = cached_instrument;
            item.period = cached_period;
            item.bar_count = bars.size();
            item.first_timestamp_ms = bars.front().timestamp_ms;
            item.last_timestamp_ms = bars.back().timestamp_ms;
            items.push_back(std::move(item));
        }
    }

    std::sort(items.begin(), items.end(), [](const KlineCatalogItem& left, const KlineCatalogItem& right) {
        if (left.instrument != right.instrument) {
            return left.instrument < right.instrument;
        }
        return left.period < right.period;
    });
    return items;
}

// Import K-line bars from CSV file, merge into existing data (从 CSV 文件导入 K 线, 合并到现有数据)
bool KlineManager::import_csv(const std::string& instrument,
                              const std::string& period,
                              const std::string& csv_path,
                              bool replace_existing,
                              size_t* imported_count,
                              std::string* error) {
    const std::string normalized_instrument = trim_copy(instrument);
    if (normalized_instrument.empty()) {
        if (error) *error = "missing_instrument";
        return false;
    }

    const std::string normalized_period = normalize_kline_period(trim_copy(period));
    const std::string normalized_path = trim_copy(csv_path);
    if (normalized_path.empty()) {
        if (error) *error = "missing_csv_path";
        return false;
    }

    std::vector<KlineBar> imported_rows;
    if (!read_import_csv_bars(normalized_path, &imported_rows, error)) {
        return false;
    }

    const std::vector<KlineBar> base_bars = aggregate_bars_to_period(imported_rows, normalized_period);
    if (base_bars.empty()) {
        if (error) *error = "csv_no_usable_rows";
        return false;
    }

    std::map<std::string, std::vector<KlineBar>> rebuilt_periods;
    for (const auto& target_period : collect_import_periods(normalized_period)) {
        rebuilt_periods[target_period] = aggregate_bars_to_period(base_bars, target_period);
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto& state = cache_[normalized_instrument];
        for (const auto& [target_period, bars] : rebuilt_periods) {
            const auto existing_it = state.bars_by_period.find(target_period);
            const std::deque<KlineBar>* existing = existing_it != state.bars_by_period.end() ? &existing_it->second : nullptr;
            const auto merged = merge_period_bars(existing, bars, replace_existing);
            state.bars_by_period[target_period] = std::deque<KlineBar>(merged.begin(), merged.end());
        }
    }

    // NOTE: request_async_save() is NOT called here.
    // The TradingEngine delegation method handles that.

    if (imported_count) {
        *imported_count = base_bars.size();
    }
    if (error) {
        error->clear();
    }
    return true;
}

// Load persisted K-line store from disk — binary format with delta encoding (从磁盘加载持久化 K 线存储 — 二进制格式, 差分编码)
bool KlineManager::load_store() {
    std::ifstream ifs(store_path_, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    char magic[sizeof(kKlineStoreMagic) - 1]{};
    if (!ifs.read(magic, static_cast<std::streamsize>(sizeof(magic))) ||
        std::string(magic, sizeof(magic)) != std::string(kKlineStoreMagic, sizeof(kKlineStoreMagic) - 1)) {
        return false;
    }

    uint64_t instrument_count = 0;
    if (!read_varuint(ifs, &instrument_count)) {
        LOG_ERROR("K 线压缩库读取失败: instrument_count: instrument_count");
        return false;
    }

    std::map<std::string, InstrumentKlineState> restored_cache;
    for (uint64_t instrument_index = 0; instrument_index < instrument_count; ++instrument_index) {
        std::string instrument;
        uint64_t period_count = 0;
        if (!read_string(ifs, &instrument) || !read_varuint(ifs, &period_count)) {
            LOG_ERROR("K 线压缩库读取失败: instrument descriptor: instrument descriptor");
            return false;
        }

        auto& state = restored_cache[instrument];
        for (uint64_t period_index = 0; period_index < period_count; ++period_index) {
            std::string period;
            uint64_t bar_count = 0;
            if (!read_string(ifs, &period) || !read_varuint(ifs, &bar_count)) {
                LOG_ERROR("K 线压缩库读取失败: period descriptor: period descriptor");
                return false;
            }

            auto& bars = state.bars_by_period[period];
            int64_t previous_timestamp = 0;
            int64_t previous_close = 0;
            int64_t previous_turnover = 0;

            for (uint64_t bar_index = 0; bar_index < bar_count; ++bar_index) {
                uint64_t timestamp_delta = 0;
                int64_t open_delta = 0;
                int64_t high_offset = 0;
                int64_t low_offset = 0;
                int64_t close_offset = 0;
                uint64_t volume = 0;
                int64_t turnover_delta = 0;
                if (!read_varuint(ifs, &timestamp_delta) ||
                    !read_varint(ifs, &open_delta) ||
                    !read_varint(ifs, &high_offset) ||
                    !read_varint(ifs, &low_offset) ||
                    !read_varint(ifs, &close_offset) ||
                    !read_varuint(ifs, &volume) ||
                    !read_varint(ifs, &turnover_delta)) {
                    LOG_ERROR("K 线压缩库读取失败: bar payload: bar payload");
                    return false;
                }

                const int64_t timestamp_ms = bar_index == 0
                    ? static_cast<int64_t>(timestamp_delta)
                    : previous_timestamp + static_cast<int64_t>(timestamp_delta);
                const int64_t open_fixed = bar_index == 0
                    ? open_delta
                    : previous_close + open_delta;
                const int64_t close_fixed = open_fixed + close_offset;
                const int64_t turnover_fixed = bar_index == 0
                    ? turnover_delta
                    : previous_turnover + turnover_delta;

                KlineBar bar;
                bar.timestamp_ms = timestamp_ms;
                bar.time = format_kline_time(timestamp_ms, period);
                bar.open = restore_price(open_fixed);
                bar.high = restore_price(open_fixed + high_offset);
                bar.low = restore_price(open_fixed + low_offset);
                bar.close = restore_price(close_fixed);
                bar.volume = static_cast<int>(volume);
                bar.turnover = restore_turnover(turnover_fixed);
                bars.push_back(std::move(bar));

                previous_timestamp = timestamp_ms;
                previous_close = close_fixed;
                previous_turnover = turnover_fixed;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        cache_.swap(restored_cache);
    }

    LOG_INFO("runtime message");
    return true;
}

// Save K-line store to disk — atomic write via temp file + rename (保存 K 线存储到磁盘 — 通过临时文件 + 重命名实现原子写入)
void KlineManager::save_store() const {
    std::map<std::string, std::map<std::string, std::vector<KlineBar>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& [instrument, state] : cache_) {
            for (const auto& [period, bars] : state.bars_by_period) {
                if (bars.empty()) {
                    continue;
                }
                const size_t count = (std::min)(bars.size(), kPersistedKlineBarsPerPeriod);
                auto& copy = snapshot[instrument][period];
                copy.reserve(count);
                for (size_t index = bars.size() - count; index < bars.size(); ++index) {
                    copy.push_back(bars[index]);
                }
            }
        }
    }

    const std::filesystem::path tmp_path = store_path_.string() + ".tmp";
    std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        LOG_INFO("runtime message");
        return;
    }

    ofs.write(kKlineStoreMagic, static_cast<std::streamsize>(sizeof(kKlineStoreMagic) - 1));
    if (!write_varuint(ofs, snapshot.size())) {
        LOG_ERROR("K 线压缩库保存失败: instrument_count: instrument_count");
        return;
    }

    for (const auto& [instrument, periods] : snapshot) {
        if (!write_string(ofs, instrument) || !write_varuint(ofs, periods.size())) {
            LOG_ERROR("K 线压缩库保存失败: instrument descriptor: instrument descriptor");
            return;
        }

        for (const auto& [period, bars] : periods) {
            if (!write_string(ofs, period) || !write_varuint(ofs, bars.size())) {
                LOG_ERROR("K 线压缩库保存失败: period descriptor: period descriptor");
                return;
            }

            int64_t previous_timestamp = 0;
            int64_t previous_close = 0;
            int64_t previous_turnover = 0;
            for (size_t index = 0; index < bars.size(); ++index) {
                const auto& bar = bars[index];
                const int64_t open_fixed = fixed_price(bar.open);
                const int64_t close_fixed = fixed_price(bar.close);
                const int64_t high_offset = fixed_price(bar.high) - open_fixed;
                const int64_t low_offset = fixed_price(bar.low) - open_fixed;
                const int64_t close_offset = close_fixed - open_fixed;
                const int64_t turnover_fixed = fixed_turnover(bar.turnover);
                const uint64_t timestamp_delta = index == 0
                    ? static_cast<uint64_t>(bar.timestamp_ms)
                    : static_cast<uint64_t>(bar.timestamp_ms - previous_timestamp);
                const int64_t open_delta = index == 0 ? open_fixed : open_fixed - previous_close;
                const int64_t turnover_delta = index == 0 ? turnover_fixed : turnover_fixed - previous_turnover;

                if (!write_varuint(ofs, timestamp_delta) ||
                    !write_varint(ofs, open_delta) ||
                    !write_varint(ofs, high_offset) ||
                    !write_varint(ofs, low_offset) ||
                    !write_varint(ofs, close_offset) ||
                    !write_varuint(ofs, static_cast<uint64_t>((std::max)(bar.volume, 0))) ||
                    !write_varint(ofs, turnover_delta)) {
                    LOG_ERROR("K 线压缩库保存失败: bar payload: bar payload");
                    return;
                }

                previous_timestamp = bar.timestamp_ms;
                previous_close = close_fixed;
                previous_turnover = turnover_fixed;
            }
        }
    }

    ofs.flush();
    ofs.close();

    std::error_code ec;
    std::filesystem::rename(tmp_path, store_path_, ec);
    if (ec) {
        LOG_INFO("runtime message");
        std::filesystem::copy_file(tmp_path, store_path_,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp_path, ec);
    }
}

void KlineManager::restore_legacy_bars(std::vector<std::tuple<std::string, std::string, KlineBar>>& items) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& item : items) {
        auto& bars = cache_[std::get<0>(item)].bars_by_period[std::get<1>(item)];
        bars.push_back(std::move(std::get<2>(item)));
        while (bars.size() > kPersistedKlineBarsPerPeriod) {
            bars.pop_front();
        }
    }
}

} // namespace hft
