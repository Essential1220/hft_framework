// ============================================
// tick_recorder.cpp - Tick data recording implementation (行情数据录制实现)
// ============================================

#include "engine/tick_recorder.h"
#include "engine/mmap_tick_writer.h"

#include "common/binary_io.h"
#include "common/string_utils.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace hft {

TickRecorder::TickRecorder() = default;
TickRecorder::~TickRecorder() = default;

namespace {

using namespace binary_io;

constexpr char kTickRecordMagic[] = "HFTTICK1";
constexpr size_t kMaxTickRecordingQueue = 100000;

std::string now_date_text() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
    return buf;
}

std::string tick_day_key(const TickData& tick) {
    std::string day = trim_copy(tick.trading_day[0] ? tick.trading_day : tick.action_day);
    if (day.empty()) day = now_date_text();
    day.erase(std::remove_if(day.begin(), day.end(),
              [](unsigned char ch) { return !std::isdigit(ch); }), day.end());
    return day.empty() ? "unknown_day" : day;
}

bool write_tick_binary(std::ostream& os, const TickData& tick) {
    return write_string(os, tick.instrument_id) &&
           write_string(os, tick.exchange_id) &&
           write_string(os, tick.update_time) &&
           write_string(os, tick.trading_day) &&
           write_string(os, tick.action_day) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.update_millisec, 0))) &&
           write_varint(os, fixed_price(tick.last_price)) &&
           write_varint(os, fixed_price(tick.pre_close_price)) &&
           write_varint(os, fixed_price(tick.open_price)) &&
           write_varint(os, fixed_price(tick.highest_price)) &&
           write_varint(os, fixed_price(tick.lowest_price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.volume, 0))) &&
           write_varint(os, fixed_turnover(tick.turnover)) &&
           write_varint(os, fixed_turnover(tick.open_interest)) &&
           write_varint(os, fixed_price(tick.bid[0].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.bid[0].volume, 0))) &&
           write_varint(os, fixed_price(tick.ask[0].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.ask[0].volume, 0))) &&
           write_varint(os, fixed_price(tick.bid[1].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.bid[1].volume, 0))) &&
           write_varint(os, fixed_price(tick.ask[1].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.ask[1].volume, 0))) &&
           write_varint(os, fixed_price(tick.bid[2].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.bid[2].volume, 0))) &&
           write_varint(os, fixed_price(tick.ask[2].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.ask[2].volume, 0))) &&
           write_varint(os, fixed_price(tick.bid[3].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.bid[3].volume, 0))) &&
           write_varint(os, fixed_price(tick.ask[3].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.ask[3].volume, 0))) &&
           write_varint(os, fixed_price(tick.bid[4].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.bid[4].volume, 0))) &&
           write_varint(os, fixed_price(tick.ask[4].price)) &&
           write_varuint(os, static_cast<uint64_t>((std::max)(tick.ask[4].volume, 0))) &&
           write_varint(os, fixed_price(tick.upper_limit)) &&
           write_varint(os, fixed_price(tick.lower_limit));
}

std::string tick_to_jsonl(const TickData& tick) {
    char buf[512];
    const int n = std::snprintf(buf, sizeof(buf),
        "{\"instrument\":\"%s\""
        ",\"exchange\":\"%s\""
        ",\"last_price\":%.6f"
        ",\"volume\":%d"
        ",\"turnover\":%.6f"
        ",\"open_interest\":%.6f"
        ",\"bid1\":%.6f"
        ",\"bid1_vol\":%d"
        ",\"ask1\":%.6f"
        ",\"ask1_vol\":%d"
        ",\"update_time\":\"%s\""
        ",\"update_millisec\":%d"
        ",\"trading_day\":\"%s\""
        ",\"action_day\":\"%s\"}",
        tick.instrument_id, tick.exchange_id,
        tick.last_price, tick.volume, tick.turnover, tick.open_interest,
        tick.bid[0].price, tick.bid[0].volume, tick.ask[0].price, tick.ask[0].volume,
        tick.update_time, tick.update_millisec,
        tick.trading_day, tick.action_day);
    return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

} // namespace

void TickRecorder::writer_loop() {
    while (true) {
        TickData tick{};
        {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() {
                return !queue_.empty() ||
                       !writer_running_.load(std::memory_order_relaxed);
            });
            if (queue_.empty()) {
                if (!writer_running_.load(std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }
            tick = queue_.front();
            queue_.pop_front();
        }
        write_to_file(tick);
    }
}

void TickRecorder::stop_writer() {
    writer_running_.store(false, std::memory_order_relaxed);
    cv_.notify_one();
}

TickRecordingStatus TickRecorder::get_status() const {
    TickRecordingStatus status;
    status.enabled = enabled_.load(std::memory_order_relaxed);
    status.recording = active_.load(std::memory_order_relaxed);
    status.recorded_ticks = recorded_count_.load(std::memory_order_relaxed);
    status.dropped_ticks = dropped_.load(std::memory_order_relaxed);
    status.storage_path = path_.string();
    std::lock_guard<std::mutex> lock(file_mtx_);
    std::error_code ec;
    for (const auto& path : collect_files("")) {
        status.file_count++;
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) status.storage_bytes += size;
        ec.clear();
    }
    return status;
}

bool TickRecorder::start(const std::string& path, std::string* error) {
    const std::string normalized_path = trim_copy(path);
    std::lock_guard<std::mutex> lock(file_mtx_);
    if (!normalized_path.empty()) {
        path_ = normalized_path;
    }
    std::error_code ec;
    if (has_extension_ci(path_, ".jsonl")) {
        if (path_.has_parent_path()) {
            std::filesystem::create_directories(path_.parent_path(), ec);
        }
        std::ofstream ofs(path_, std::ios::app);
        if (!ofs.is_open()) {
            if (error) *error = "open_tick_record_file_failed";
            return false;
        }
    } else {
        if (std::filesystem::is_regular_file(path_)) {
            const auto existing_size = std::filesystem::file_size(path_, ec);
            if (!ec && existing_size == 0) {
                std::filesystem::remove(path_, ec);
            }
        }
        ec.clear();
        std::filesystem::create_directories(path_, ec);
        if (ec || !std::filesystem::is_directory(path_)) {
            if (error) *error = "create_tick_record_directory_failed";
            return false;
        }
    }
    enabled_.store(true, std::memory_order_relaxed);
    active_.store(true, std::memory_order_relaxed);
    if (error) error->clear();
    return true;
}

bool TickRecorder::stop(std::string* error) {
    (void)error;
    active_.store(false, std::memory_order_relaxed);
    enabled_.store(false, std::memory_order_relaxed);
    if (mmap_writer_) {
        std::lock_guard<std::mutex> lock(file_mtx_);
        mmap_writer_->close();
        mmap_writer_.reset();
        mmap_current_day_.clear();
    }
    return true;
}

bool TickRecorder::delete_files(const std::string& instrument,
                                std::string* error,
                                size_t* deleted_files,
                                uintmax_t* deleted_bytes) {
    if (deleted_files) *deleted_files = 0;
    if (deleted_bytes) *deleted_bytes = 0;
    if (active_.load(std::memory_order_relaxed)) {
        if (error) *error = "recording_active";
        return false;
    }

    const std::string filter = trim_copy(instrument);
    std::lock_guard<std::mutex> lock(file_mtx_);
    const auto files = collect_files(filter);
    std::error_code ec;
    for (const auto& p : files) {
        const bool allowed = has_extension_ci(p, ".htick") || has_extension_ci(p, ".jsonl");
        if (!allowed) continue;
        uintmax_t bytes = std::filesystem::file_size(p, ec);
        if (ec) {
            bytes = 0;
            ec.clear();
        }
        if (std::filesystem::remove(p, ec)) {
            if (deleted_files) (*deleted_files)++;
            if (deleted_bytes) (*deleted_bytes) += bytes;
        } else if (ec) {
            if (error) *error = "delete_failed:" + p.string();
            return false;
        }
        ec.clear();
    }
    return true;
}

void TickRecorder::record(const TickData& tick) {
    if (!active_.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (queue_.size() >= kMaxTickRecordingQueue) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            active_.store(false, std::memory_order_relaxed);
            enabled_.store(false, std::memory_order_relaxed);
            if (!drop_alerted_.exchange(true, std::memory_order_relaxed)) {
                if (alert_cb_) alert_cb_("tick record queue overflow, recording stopped");
            }
            return;
        }
        queue_.push_back(tick);
    }
    cv_.notify_one();
}

bool TickRecorder::write_to_file(const TickData& tick) {
    if (storage_mode_ == TickStorageMode::Mmap) {
        return write_to_mmap(tick);
    }

    std::lock_guard<std::mutex> lock(file_mtx_);
    const auto fpath = file_for(tick);
    std::error_code ec;
    if (fpath.has_parent_path()) {
        std::filesystem::create_directories(fpath.parent_path(), ec);
    }
    const bool jsonl = has_extension_ci(fpath, ".jsonl");
    std::ofstream ofs(fpath, jsonl ? std::ios::app : (std::ios::binary | std::ios::app));
    if (!ofs.is_open()) {
        active_.store(false, std::memory_order_relaxed);
        enabled_.store(false, std::memory_order_relaxed);
        if (alert_cb_) alert_cb_("tick record write failed");
        return false;
    }
    if (jsonl) {
        ofs << tick_to_jsonl(tick) << '\n';
    } else {
        if (std::filesystem::file_size(fpath, ec) == 0) {
            ofs.write(kTickRecordMagic, sizeof(kTickRecordMagic) - 1);
        }
        if (!write_tick_binary(ofs, tick)) {
            active_.store(false, std::memory_order_relaxed);
            enabled_.store(false, std::memory_order_relaxed);
            if (alert_cb_) alert_cb_("tick record encode failed");
            return false;
        }
    }
    recorded_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TickRecorder::ensure_mmap_open(const TickData& tick) {
    const std::string day = tick_day_key(tick);
    if (mmap_writer_ && mmap_current_day_ == day) return;

    if (mmap_writer_) {
        mmap_writer_->close();
        mmap_writer_.reset();
    }

    mmap_writer_ = std::make_unique<MmapTickWriter>();
    auto mmap_path = path_ / (day + ".mmap");
    if (!mmap_writer_->open(mmap_path, mmap_max_ticks_)) {
        mmap_writer_.reset();
        active_.store(false, std::memory_order_relaxed);
        enabled_.store(false, std::memory_order_relaxed);
        if (alert_cb_) alert_cb_("mmap tick writer open failed: " + mmap_path.string());
        return;
    }
    mmap_current_day_ = day;
}

bool TickRecorder::write_to_mmap(const TickData& tick) {
    std::lock_guard<std::mutex> lock(file_mtx_);
    ensure_mmap_open(tick);
    if (!mmap_writer_ || !mmap_writer_->is_open()) return false;

    if (mmap_writer_->count() >= mmap_max_ticks_) {
        active_.store(false, std::memory_order_relaxed);
        enabled_.store(false, std::memory_order_relaxed);
        if (alert_cb_) alert_cb_("mmap tick storage full");
        return false;
    }
    mmap_writer_->write(tick);
    recorded_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::filesystem::path TickRecorder::file_for(const TickData& tick) const {
    if (has_extension_ci(path_, ".jsonl")) {
        return path_;
    }
    const std::string day = tick_day_key(tick);
    const std::string instrument = safe_path_component(tick.instrument_id);
    return path_ / day / (instrument + ".htick");
}

std::vector<std::filesystem::path> TickRecorder::collect_files(const std::string& instrument) const {
    std::vector<std::filesystem::path> files;
    const std::string filter = safe_path_component(instrument);
    if (std::filesystem::is_regular_file(path_)) {
        files.push_back(path_);
        return files;
    }
    if (!std::filesystem::exists(path_)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path_)) {
        if (!entry.is_regular_file()) continue;
        const auto p = entry.path();
        const bool binary = has_extension_ci(p, ".htick");
        const bool jsonl = has_extension_ci(p, ".jsonl");
        if (!binary && !jsonl) continue;
        if (!instrument.empty() && binary && p.stem().string() != filter) continue;
        files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace hft
