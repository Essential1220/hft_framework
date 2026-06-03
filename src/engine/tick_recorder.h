#pragma once
// ============================================
// tick_recorder.h - Tick data recording to file (行情数据录制到文件)
// Records raw tick data to disk in binary (.htick) or JSONL format
// with a background writer thread and bounded queue.
// (将原始行情数据录制到磁盘, 支持二进制/JSONL 格式, 后台写线程 + 有界队列)
// ============================================

#include "common/types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace hft {

struct TickRecordingStatus {
    bool enabled = false;
    bool recording = false;
    size_t recorded_ticks = 0;
    size_t dropped_ticks = 0;
    size_t file_count = 0;
    uintmax_t storage_bytes = 0;
    std::string storage_path;
};

class TickRecorder {
public:
    using AlertCallback = std::function<void(const std::string&)>;

    void set_alert_callback(AlertCallback cb) { alert_cb_ = std::move(cb); }
    void set_path(const std::filesystem::path& path) { path_ = path; }
    const std::filesystem::path& path() const { return path_; }

    void writer_loop();
    void stop_writer();

    TickRecordingStatus get_status() const;
    bool start(const std::string& path = "", std::string* error = nullptr);
    bool stop(std::string* error = nullptr);
    bool delete_files(const std::string& instrument = "",
                      std::string* error = nullptr,
                      size_t* deleted_files = nullptr,
                      uintmax_t* deleted_bytes = nullptr);
    void record(const TickData& tick);

    bool is_enabled() const { return enabled_.load(std::memory_order_relaxed); }
    bool is_active() const { return active_.load(std::memory_order_relaxed); }
    size_t recorded_count() const { return recorded_count_.load(std::memory_order_relaxed); }
    size_t dropped_count() const { return dropped_.load(std::memory_order_relaxed); }
    std::string path_string() const { return path_.string(); }

    std::condition_variable& cv() { return cv_; }
    std::atomic<bool>& writer_running_ref() { return writer_running_; }

private:
    bool write_to_file(const TickData& tick);
    std::filesystem::path file_for(const TickData& tick) const;
    std::vector<std::filesystem::path> collect_files(const std::string& instrument) const;

    AlertCallback alert_cb_;
    std::filesystem::path path_ = "tick_records.dat";
    std::atomic<bool> enabled_{false};
    std::atomic<bool> active_{false};
    mutable std::mutex file_mtx_;
    std::atomic<bool> writer_running_{false};
    std::mutex queue_mtx_;
    std::condition_variable cv_;
    std::deque<TickData> queue_;
    std::atomic<size_t> dropped_{0};
    std::atomic<bool> drop_alerted_{false};
    std::atomic<size_t> recorded_count_{0};
};

} // namespace hft
