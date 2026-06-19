#pragma once
// ============================================
// async_logger.h - Lock-free async logging system (无锁异步日志系统)
//
// Goals:
// 1. Lock-free MPSC queue: multiple producer threads push via atomic CAS,
//    single consumer thread drains and writes to disk/console.
// 2. Fixed-size ring buffer: no heap allocation on the hot path.
// 3. Support level filtering, recent log retention, daily rotation and expiry cleanup.
// ============================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <ctime>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace hft {

enum class LogLevel : uint8_t {
    Info = 0,
    Warn = 1,
    Error = 2
};

inline const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

inline LogLevel parse_log_level(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (text == "WARN" || text == "WARNING" || text == "警告") return LogLevel::Warn;
    if (text == "ERROR" || text == "ERR" || text == "错误") return LogLevel::Error;
    return LogLevel::Info;
}

inline uint32_t current_thread_id() {
#ifdef _WIN32
    return static_cast<uint32_t>(::GetCurrentThreadId());
#else
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

#ifdef _WIN32
inline bool write_console_utf8_line(const std::string& text) {
    HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD console_mode = 0;
    if (!::GetConsoleMode(handle, &console_mode)) {
        return false;
    }

    const int wide_size = ::MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (wide_size <= 0) {
        return false;
    }

    std::wstring wide_text(static_cast<size_t>(wide_size), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            wide_text.data(), wide_size) <= 0) {
        return false;
    }

    DWORD written = 0;
    if (!::WriteConsoleW(handle, wide_text.c_str(), static_cast<DWORD>(wide_text.size()), &written, nullptr)) {
        return false;
    }

    static constexpr wchar_t kNewline[] = L"\n";
    return ::WriteConsoleW(handle, kNewline, 1, &written, nullptr) != FALSE;
}
#endif

struct LoggerOptions {
    std::string log_dir = "logs";
    std::string file_prefix = "hft";
    size_t queue_capacity = 8192;
    size_t recent_capacity = 400;
    int flush_interval_ms = 1000;
    int retention_days = 7;
    LogLevel min_level = LogLevel::Info;
};

struct RecentLogEntry {
    std::string timestamp;
    std::string level;
    uint32_t thread_id = 0;
    std::string message;
};

struct LoggerStats {
    uint64_t dropped_messages = 0;
    uint64_t written_messages = 0;
    uint64_t error_count = 0;
    uint64_t warn_count = 0;
    size_t queued_messages = 0;
    size_t recent_messages = 0;
    std::string current_file;
    std::string log_directory;
    std::string min_level;
};

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level = LogLevel::Info;
    uint32_t thread_id = 0;
    std::string message;
};

class AsyncLogger {
public:
    static AsyncLogger& instance() {
        static AsyncLogger inst;
        return inst;
    }

    void start(LoggerOptions options = LoggerOptions{}) {
        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            if (running_) return;
            options_ = normalize_options(std::move(options));
        }

        dropped_messages_.store(0, std::memory_order_relaxed);
        written_messages_.store(0, std::memory_order_relaxed);
        error_count_.store(0, std::memory_order_relaxed);
        warn_count_.store(0, std::memory_order_relaxed);
        write_head_.store(0, std::memory_order_relaxed);
        read_tail_.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < kRingCap; ++i) {
            ring_[i].state.store(0, std::memory_order_relaxed);
        }

#ifdef _WIN32
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
#endif

        open_log_file_for(std::chrono::system_clock::now());
        running_ = true;
        worker_thread_ = std::thread(&AsyncLogger::consumer_loop, this);
    }

    void stop() {
        if (!running_) {
            close_log_file();
            return;
        }

        running_ = false;
        wake_consumer();

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        flush_file();
        close_log_file();
    }

    ~AsyncLogger() {
        stop();
    }

    void log(LogLevel level, const char* fmt, ...) {
        if (!running_ || static_cast<uint8_t>(level) < static_cast<uint8_t>(options_.min_level)) {
            return;
        }

        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        enqueue_fixed(level, current_thread_id(), std::chrono::system_clock::now(), buffer);
    }

    void log(uint8_t level, const char* fmt, ...) {
        if (!running_) return;

        const LogLevel typed_level =
            level == 0 ? LogLevel::Info :
            level == 1 ? LogLevel::Warn :
                         LogLevel::Error;

        if (static_cast<uint8_t>(typed_level) < static_cast<uint8_t>(options_.min_level)) {
            return;
        }

        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        enqueue_fixed(typed_level, current_thread_id(), std::chrono::system_clock::now(), buffer);
    }

    bool should_log(LogLevel level) const {
        return running_ && static_cast<uint8_t>(level) >= static_cast<uint8_t>(options_.min_level);
    }

    void log_string(LogLevel level, const std::string& message) {
        if (!running_ || static_cast<uint8_t>(level) < static_cast<uint8_t>(options_.min_level)) {
            return;
        }

        enqueue_fixed(level, current_thread_id(), std::chrono::system_clock::now(), message.c_str());
    }

    std::vector<RecentLogEntry> recent_entries(size_t limit = 200) const {
        std::lock_guard<std::mutex> lock(recent_mtx_);
        std::vector<RecentLogEntry> out;
        const size_t take = std::min(limit, recent_entries_.size());
        out.reserve(take);
        for (size_t i = 0; i < take; ++i) {
            out.push_back(recent_entries_[recent_entries_.size() - take + i]);
        }
        return out;
    }

    void clear_recent_entries() {
        std::lock_guard<std::mutex> lock(recent_mtx_);
        recent_entries_.clear();
    }

    LoggerStats stats() const {
        LoggerStats s;
        s.dropped_messages = dropped_messages_.load(std::memory_order_relaxed);
        s.written_messages = written_messages_.load(std::memory_order_relaxed);
        s.error_count = error_count_.load(std::memory_order_relaxed);
        s.warn_count = warn_count_.load(std::memory_order_relaxed);
        s.min_level = log_level_name(options_.min_level);
        s.log_directory = options_.log_dir;

        {
            const size_t h = write_head_.load(std::memory_order_relaxed);
            const size_t t = read_tail_.load(std::memory_order_relaxed);
            s.queued_messages = (h >= t) ? (h - t) : 0;
        }
        {
            std::lock_guard<std::mutex> recent_lock(recent_mtx_);
            s.recent_messages = recent_entries_.size();
        }
        {
            std::lock_guard<std::mutex> state_lock(state_mtx_);
            s.current_file = current_file_;
        }

        return s;
    }

    std::thread& worker_thread() {
        return worker_thread_;
    }

private:
    AsyncLogger() = default;
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    // ---- Lock-free MPSC ring buffer ----
    static constexpr size_t kRingCap = 8192;
    static constexpr size_t kRingMask = kRingCap - 1;
    static constexpr size_t kMaxMsgLen = 504;

    static constexpr uint8_t kSlotEmpty   = 0;
    static constexpr uint8_t kSlotWriting = 1;
    static constexpr uint8_t kSlotReady   = 2;

    struct alignas(64) RingSlot {
        std::atomic<uint8_t> state{0};
        LogLevel level = LogLevel::Info;
        uint32_t thread_id = 0;
        std::chrono::system_clock::time_point timestamp;
        char message[kMaxMsgLen]{};
    };

    alignas(64) std::atomic<size_t> write_head_{0};
    alignas(64) std::atomic<size_t> read_tail_{0};
    RingSlot ring_[kRingCap]{};

    void enqueue_fixed(LogLevel level, uint32_t tid,
                       std::chrono::system_clock::time_point ts,
                       const char* msg) {
        size_t head = write_head_.load(std::memory_order_relaxed);
        for (;;) {
            const size_t tail = read_tail_.load(std::memory_order_acquire);
            if (head - tail >= kRingCap) {
                if (level == LogLevel::Info) {
                    dropped_messages_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
                head = write_head_.load(std::memory_order_relaxed);
                continue;
            }
            if (write_head_.compare_exchange_weak(head, head + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                break;
            }
        }

        RingSlot& slot = ring_[head & kRingMask];
        slot.state.store(kSlotWriting, std::memory_order_relaxed);
        slot.level = level;
        slot.thread_id = tid;
        slot.timestamp = ts;
        std::strncpy(slot.message, msg, kMaxMsgLen - 1);
        slot.message[kMaxMsgLen - 1] = '\0';
        slot.state.store(kSlotReady, std::memory_order_release);

        wake_consumer();
    }

    bool has_pending_entries() const {
        const size_t t = read_tail_.load(std::memory_order_relaxed);
        return ring_[t & kRingMask].state.load(std::memory_order_acquire) == kSlotReady;
    }

    // ---- Consumer wake mechanism ----
    std::mutex cv_mtx_;
    std::condition_variable queue_cv_;

    void wake_consumer() {
        queue_cv_.notify_one();
    }

    // ---- Config helpers ----
    static LoggerOptions normalize_options(LoggerOptions options) {
        if (options.log_dir.empty()) options.log_dir = "logs";
        if (options.file_prefix.empty()) options.file_prefix = "hft";
        if (options.queue_capacity == 0) options.queue_capacity = 8192;
        if (options.recent_capacity == 0) options.recent_capacity = 400;
        if (options.flush_interval_ms <= 0) options.flush_interval_ms = 1000;
        if (options.retention_days < 0) options.retention_days = 0;
        return options;
    }

    static std::tm local_time(std::time_t t) {
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        return tm_buf;
    }

    static std::string format_date_token(std::chrono::system_clock::time_point tp) {
        const auto time = std::chrono::system_clock::to_time_t(tp);
        const std::tm tm_buf = local_time(time);
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
        return buffer;
    }

    static std::string format_timestamp(std::chrono::system_clock::time_point tp) {
        const auto time = std::chrono::system_clock::to_time_t(tp);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()).count() % 1000;
        const std::tm tm_buf = local_time(time);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                      static_cast<int>(ms));
        return buffer;
    }

    // ---- Format + write ----
    std::string format_slot(const RingSlot& slot) const {
        std::string line = "[";
        line += format_timestamp(slot.timestamp);
        line += "][";
        line += log_level_name(slot.level);
        line += "][T";
        line += std::to_string(slot.thread_id);
        line += "] ";
        line += slot.message;
        return line;
    }

    void write_slot(const RingSlot& slot) {
        rotate_if_needed(slot.timestamp);

        const std::string line = format_slot(slot);
#ifdef _WIN32
        if (!write_console_utf8_line(line)) {
            std::fprintf(stdout, "%s\n", line.c_str());
        }
#else
        std::fprintf(stdout, "%s\n", line.c_str());
#endif

        if (fp_ != nullptr) {
            std::fprintf(fp_, "%s\n", line.c_str());
            if (slot.level != LogLevel::Info) {
                std::fflush(fp_);
            }
        }

        {
            std::lock_guard<std::mutex> lock(recent_mtx_);
            recent_entries_.push_back(RecentLogEntry{
                format_timestamp(slot.timestamp),
                log_level_name(slot.level),
                slot.thread_id,
                slot.message
            });
            while (recent_entries_.size() > options_.recent_capacity) {
                recent_entries_.pop_front();
            }
        }

        written_messages_.fetch_add(1, std::memory_order_relaxed);
        if (slot.level == LogLevel::Error) error_count_.fetch_add(1, std::memory_order_relaxed);
        else if (slot.level == LogLevel::Warn) warn_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- Consumer loop ----
    void consumer_loop() {
        const auto flush_interval = std::chrono::milliseconds(options_.flush_interval_ms);
        auto next_flush = std::chrono::steady_clock::now() + flush_interval;
        uint64_t last_reported_drops = 0;

        while (running_ || has_pending_entries()) {
            size_t consumed = 0;
            size_t tail = read_tail_.load(std::memory_order_relaxed);

            while (consumed < 256) {
                RingSlot& slot = ring_[tail & kRingMask];
                if (slot.state.load(std::memory_order_acquire) != kSlotReady) {
                    break;
                }
                write_slot(slot);
                slot.state.store(kSlotEmpty, std::memory_order_release);
                ++tail;
                ++consumed;
            }
            read_tail_.store(tail, std::memory_order_release);

            if (std::chrono::steady_clock::now() >= next_flush) {
                const uint64_t total_drops = dropped_messages_.load(std::memory_order_relaxed);
                if (total_drops > last_reported_drops) {
                    const uint64_t new_drops = total_drops - last_reported_drops;
                    last_reported_drops = total_drops;
                    char drop_msg[128];
                    std::snprintf(drop_msg, sizeof(drop_msg),
                                  "log queue overflow: %llu messages dropped (total=%llu)",
                                  static_cast<unsigned long long>(new_drops),
                                  static_cast<unsigned long long>(total_drops));
                    RingSlot synthetic{};
                    synthetic.level = LogLevel::Warn;
                    synthetic.thread_id = current_thread_id();
                    synthetic.timestamp = std::chrono::system_clock::now();
                    std::strncpy(synthetic.message, drop_msg, kMaxMsgLen - 1);
                    write_slot(synthetic);
                }
                flush_file();
                next_flush = std::chrono::steady_clock::now() + flush_interval;
            }

            if (consumed == 0 && running_) {
                std::unique_lock<std::mutex> lock(cv_mtx_);
                queue_cv_.wait_until(lock, next_flush, [this] {
                    return has_pending_entries() || !running_;
                });
            }
        }

        flush_file();
    }

    // ---- File rotation ----
    void rotate_if_needed(std::chrono::system_clock::time_point tp) {
        const std::string token = format_date_token(tp);
        if (token == current_date_token_) return;
        flush_file();
        close_log_file();
        open_log_file_for(tp);
    }

    void open_log_file_for(std::chrono::system_clock::time_point tp) {
        namespace fs = std::filesystem;
        const std::string date_token = format_date_token(tp);
        const fs::path log_dir = fs::path(options_.log_dir);

        std::error_code ec;
        fs::create_directories(log_dir, ec);

        const fs::path file_path = log_dir / (options_.file_prefix + "_" + date_token + ".log");
        const bool existed = fs::exists(file_path, ec);

        fp_ = std::fopen(file_path.string().c_str(), "ab");
        if (fp_ != nullptr && !existed) {
#ifdef _WIN32
            const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
            std::fwrite(bom, 1, sizeof(bom), fp_);
#endif
        }

        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            current_date_token_ = date_token;
            current_file_ = file_path.string();
        }

        cleanup_expired_logs(tp);
    }

    void cleanup_expired_logs(std::chrono::system_clock::time_point now) {
        if (options_.retention_days <= 0) return;
        namespace fs = std::filesystem;
        const auto cutoff = now - std::chrono::hours(24LL * options_.retention_days);
        const std::string cutoff_token = format_date_token(cutoff);
        const fs::path log_dir = fs::path(options_.log_dir);

        std::error_code ec;
        if (!fs::exists(log_dir, ec)) return;

        for (const auto& entry : fs::directory_iterator(log_dir, ec)) {
            if (ec || !entry.is_regular_file()) continue;
            const std::string filename = entry.path().filename().string();
            const std::string prefix = options_.file_prefix + "_";
            if (filename.rfind(prefix, 0) != 0 || entry.path().extension() != ".log") {
                continue;
            }
            if (filename.size() < prefix.size() + 12) continue;
            const std::string date_token = filename.substr(prefix.size(), 8);
            if (date_token < cutoff_token) {
                fs::remove(entry.path(), ec);
            }
        }
    }

    void flush_file() {
        if (fp_ != nullptr) {
            std::fflush(fp_);
        }
    }

    void close_log_file() {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
        current_file_.clear();
        current_date_token_.clear();
    }

    LoggerOptions options_;

    mutable std::mutex recent_mtx_;
    std::deque<RecentLogEntry> recent_entries_;

    mutable std::mutex state_mtx_;
    std::atomic<uint64_t> dropped_messages_{0};
    std::atomic<uint64_t> written_messages_{0};
    std::atomic<uint64_t> error_count_{0};
    std::atomic<uint64_t> warn_count_{0};
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    FILE* fp_ = nullptr;
    std::string current_date_token_;
    std::string current_file_;
};

#define ASYNC_LOG_INFO(fmt, ...)  hft::AsyncLogger::instance().log(hft::LogLevel::Info, fmt, ##__VA_ARGS__)
#define ASYNC_LOG_WARN(fmt, ...)  hft::AsyncLogger::instance().log(hft::LogLevel::Warn, fmt, ##__VA_ARGS__)
#define ASYNC_LOG_ERROR(fmt, ...) hft::AsyncLogger::instance().log(hft::LogLevel::Error, fmt, ##__VA_ARGS__)

} // namespace hft
