#pragma once
// ============================================
// async_logger.h - Async logging system (异步日志系统)
//
// Goals (目标)：
// 1. Allow multiple threads to safely write logs (允许多个线程安全写日志)
// 2. Background thread async disk flush to reduce business thread I/O jitter (后台线程异步落盘，降低业务线程 I/O 抖动)
// 3. Support level filtering, recent log retention, daily rotation and expiry cleanup
//    (支持级别过滤、保留最近日志、按天滚动和过期清理)
// ============================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
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
    Info = 0,   // Informational (信息级别)
    Warn = 1,   // Warning (警告级别)
    Error = 2   // Error (错误级别)
};

// Convert log level to string name (将日志级别转换为字符串名称)
inline const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO"; // Default returns INFO (默认返回 INFO)
}

// Parse log level string to enum type (解析字符串格式的日志级别，将其转换为枚举类型)
inline LogLevel parse_log_level(std::string text) {
    // Convert string to uppercase for case-insensitive comparison (将字符串转换为大写以实现不区分大小写的比较)
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (text == "WARN" || text == "WARNING" || text == "警告") return LogLevel::Warn;
    if (text == "ERROR" || text == "ERR" || text == "错误") return LogLevel::Error;
    return LogLevel::Info; // Default to Info (默认级别为 Info)
}

// Get current thread ID (获取当前线程 ID)
inline uint32_t current_thread_id() {
#ifdef _WIN32
    return static_cast<uint32_t>(::GetCurrentThreadId()); // Windows: GetCurrentThreadId (Windows 下获取线程 ID)
#else
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())); // Linux/Unix: hash thread ID (Linux/Unix 下对线程 ID 进行哈希)
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
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (wide_size <= 0) {
        return false;
    }

    std::wstring wide_text(static_cast<size_t>(wide_size), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8,
            0,
            text.c_str(),
            static_cast<int>(text.size()),
            wide_text.data(),
            wide_size) <= 0) {
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

// Log configuration options struct (日志配置选项结构体)
struct LoggerOptions {
    std::string log_dir = "logs";         // Log file directory (日志文件存储目录)
    std::string file_prefix = "hft";      // Log filename prefix (日志文件名前缀)
    size_t queue_capacity = 8192;         // Max async queue capacity (异步日志队列最大容量)
    size_t recent_capacity = 400;         // Recent log entries kept in memory for web query (在内存中保留最近日志的条数，供 Web 查询)
    int flush_interval_ms = 1000;         // Log flush interval in milliseconds (日志刷盘时间间隔，毫秒)
    int retention_days = 7;               // Log retention days (日志保留天数)
    LogLevel min_level = LogLevel::Info;  // Minimum log level to record (最小记录日志级别)
};

// Recent log entry for web API response (用于 Web 接口返回的近期日志条目结构体)
struct RecentLogEntry {
    std::string timestamp;  // Formatted timestamp (格式化后的时间戳)
    std::string level;      // Log level string (日志级别字符串)
    uint32_t thread_id = 0; // Thread ID (线程 ID)
    std::string message;    // Log content (日志内容)
};

// Logger statistics struct (日志系统统计信息结构体)
struct LoggerStats {
    uint64_t dropped_messages = 0;  // Dropped messages count (queue full) (丢弃的日志消息数量，队列满时)
    uint64_t written_messages = 0;  // Successfully written messages (成功写入的日志消息数量)
    uint64_t error_count = 0;       // Cumulative ERROR count (累计 ERROR 级别日志数量)
    uint64_t warn_count = 0;        // Cumulative WARN count (累计 WARN 级别日志数量)
    size_t queued_messages = 0;     // Pending messages in queue (当前队列中待处理的日志消息数量)
    size_t recent_messages = 0;     // Cached recent entries count (当前缓存的近期日志条数)
    std::string current_file;       // Current log file path (当前正在写入的日志文件路径)
    std::string log_directory;      // Log directory (日志目录)
    std::string min_level;          // Current minimum log level (当前最小日志级别)
};

// Single log entry in the queue (日志队列中的单条日志记录)
struct LogEntry {
    std::chrono::system_clock::time_point timestamp; // Generation time (产生时间)
    LogLevel level = LogLevel::Info;                 // Log level (日志级别)
    uint32_t thread_id = 0;                          // Thread ID (线程 ID)
    std::string message;                             // Log content (日志内容)
};

class AsyncLogger {
public:
    // Get singleton instance (获取单例实例)
    static AsyncLogger& instance() {
        static AsyncLogger inst;
        return inst;
    }

    // Start the logging system (启动日志系统)
    void start(LoggerOptions options = LoggerOptions{}) {
        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            if (running_) return; // Already started, return directly (已经启动则直接返回)
            options_ = normalize_options(std::move(options)); // Normalize options (标准化配置选项)
        }

        // Initialize statistics counters (初始化统计计数)
        dropped_messages_.store(0, std::memory_order_relaxed);
        written_messages_.store(0, std::memory_order_relaxed);
        error_count_.store(0, std::memory_order_relaxed);
        warn_count_.store(0, std::memory_order_relaxed);

#ifdef _WIN32
        // Set console output to UTF-8 on Windows for Chinese support (Windows 环境下设置控制台编码为 UTF-8，以支持中文输出)
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
#endif

        // Open/rotate today's log file (创建并打开当天的日志文件)
        open_log_file_for(std::chrono::system_clock::now());
        running_ = true; // Set running flag (设置运行标志位)
        // Start background consumer thread (启动后台消费线程)
        worker_thread_ = std::thread(&AsyncLogger::consumer_loop, this);
    }

    // Stop the logging system (停止日志系统)
    void stop() {
        if (!running_) {
            close_log_file();
            return;
        }

        running_ = false; // Clear running flag (清除运行标志位)
        queue_cv_.notify_all(); // Wake consumer thread that might be waiting (唤醒可能在等待的消费线程)

        // Wait for consumer thread to finish (等待消费线程结束)
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        // Flush and close file (刷写并关闭文件)
        flush_file();
        close_log_file();
    }

    // Destructor auto-stops the logging system (析构时自动停止日志系统)
    ~AsyncLogger() {
        stop();
    }

    // Formatted log output (格式化输出日志)
    void log(LogLevel level, const char* fmt, ...) {
        // If not running or level below minimum, return directly (如果未运行或日志级别低于最低级别，则直接返回)
        if (!running_ || static_cast<uint8_t>(level) < static_cast<uint8_t>(options_.min_level)) {
            return;
        }

        // Use varargs to format string (使用变长参数格式化字符串)
        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        // Enqueue the log entry (将日志入队)
        enqueue(LogEntry{
            std::chrono::system_clock::now(),
            level,
            current_thread_id(),
            buffer
        });
    }

    // Formatted log output for numeric level (compatibility, e.g. Python bindings) (兼容数字级别的格式化日志输出，供 Python 绑定等使用)
    void log(uint8_t level, const char* fmt, ...) {
        if (!running_) return;

        // Convert numeric level to enum type (转换数字级别为枚举类型)
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

        enqueue(LogEntry{
            std::chrono::system_clock::now(),
            typed_level,
            current_thread_id(),
            buffer
        });
    }

    // Log a raw string message (记录直接字符串的日志)
    void log_string(LogLevel level, const std::string& message) {
        if (!running_ || static_cast<uint8_t>(level) < static_cast<uint8_t>(options_.min_level)) {
            return;
        }

        enqueue(LogEntry{
            std::chrono::system_clock::now(),
            level,
            current_thread_id(),
            message
        });
    }

    // Get recent log entries for web API etc. (获取最近的日志条目，供 Web API 等查询)
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

    // Clear web recent log cache only, does NOT delete disk log files (清空 Web 端近期日志缓存，不删除磁盘日志文件)
    void clear_recent_entries() {
        std::lock_guard<std::mutex> lock(recent_mtx_);
        recent_entries_.clear();
    }

    // Get current logger statistics (获取日志系统当前统计信息)
    LoggerStats stats() const {
        LoggerStats s;
        s.dropped_messages = dropped_messages_.load(std::memory_order_relaxed);
        s.written_messages = written_messages_.load(std::memory_order_relaxed);
        s.error_count = error_count_.load(std::memory_order_relaxed);
        s.warn_count = warn_count_.load(std::memory_order_relaxed);
        s.min_level = log_level_name(options_.min_level);
        s.log_directory = options_.log_dir;

        {
            std::lock_guard<std::mutex> queue_lock(queue_mtx_);
            s.queued_messages = queue_.size(); // Get queue backlog count (获取队列堆积数量)
        }
        {
            std::lock_guard<std::mutex> recent_lock(recent_mtx_);
            s.recent_messages = recent_entries_.size(); // Get cached recent log count (获取缓存的近期日志数量)
        }
        {
            std::lock_guard<std::mutex> state_lock(state_mtx_);
            s.current_file = current_file_; // Get current log file path (获取当前写入的日志文件)
        }

        return s;
    }

    // Get worker thread handle (for core pinning etc.) (获取工作线程句柄，可用于绑核等操作)
    std::thread& worker_thread() {
        return worker_thread_;
    }

private:
    AsyncLogger() = default;
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    // Normalize config options (handle defaults) (规范化配置选项，处理默认值)
    static LoggerOptions normalize_options(LoggerOptions options) {
        if (options.log_dir.empty()) options.log_dir = "logs";
        if (options.file_prefix.empty()) options.file_prefix = "hft";
        if (options.queue_capacity == 0) options.queue_capacity = 8192;
        if (options.recent_capacity == 0) options.recent_capacity = 400;
        if (options.flush_interval_ms <= 0) options.flush_interval_ms = 1000;
        if (options.retention_days < 0) options.retention_days = 0;
        return options;
    }

    // Cross-platform localtime (跨平台的本地时间获取)
    static std::tm local_time(std::time_t t) {
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t); // Windows safe version (Windows 安全版本)
#else
        localtime_r(&t, &tm_buf); // POSIX thread-safe version (POSIX 线程安全版本)
#endif
        return tm_buf;
    }

    // Format date token for log rotation (e.g. 20240325) (格式化日期字符串，用于日志滚动，如 20240325)
    static std::string format_date_token(std::chrono::system_clock::time_point tp) {
        const auto time = std::chrono::system_clock::to_time_t(tp);
        const std::tm tm_buf = local_time(time);
        char buffer[16];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%04d%02d%02d",
            tm_buf.tm_year + 1900,
            tm_buf.tm_mon + 1,
            tm_buf.tm_mday
        );
        return buffer;
    }

    // Format timestamp with milliseconds (格式化带毫秒的时间戳)
    static std::string format_timestamp(std::chrono::system_clock::time_point tp) {
        const auto time = std::chrono::system_clock::to_time_t(tp);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()).count() % 1000;
        const std::tm tm_buf = local_time(time);
        char buffer[32];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%04d-%02d-%02d %02d:%02d:%02d.%03d",
            tm_buf.tm_year + 1900,
            tm_buf.tm_mon + 1,
            tm_buf.tm_mday,
            tm_buf.tm_hour,
            tm_buf.tm_min,
            tm_buf.tm_sec,
            static_cast<int>(ms)
        );
        return buffer;
    }

    // Push log entry into the queue (将日志条目推入队列)
    void enqueue(LogEntry&& entry) {
        std::unique_lock<std::mutex> lock(queue_mtx_);
        if (queue_.size() >= options_.queue_capacity) {
            if (entry.level == LogLevel::Info) {
                dropped_messages_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // WARN/ERROR: evict oldest Info entry to make room
            auto it = std::find_if(queue_.begin(), queue_.end(),
                [](const LogEntry& e) { return e.level == LogLevel::Info; });
            if (it != queue_.end()) {
                queue_.erase(it);
                dropped_messages_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        queue_.push_back(std::move(entry));
        lock.unlock();
        queue_cv_.notify_one(); // Wake consumer thread (唤醒消费线程)
    }

    // Background consumer thread main loop (后台消费线程主循环)
    void consumer_loop() {
        const auto flush_interval = std::chrono::milliseconds(options_.flush_interval_ms);
        auto next_flush = std::chrono::steady_clock::now() + flush_interval;
        uint64_t last_reported_drops = 0;

        // Keep processing while running or queue has pending data (只要还在运行或队列中仍有数据，就继续处理)
        while (running_ || has_pending_entries()) {
            std::deque<LogEntry> batch;

            {
                std::unique_lock<std::mutex> lock(queue_mtx_);
                // Wait until queue non-empty, stop requested, or next flush time (等待直到队列非空、停止运行或到达下一次刷盘时间)
                queue_cv_.wait_until(lock, next_flush, [this] {
                    return !queue_.empty() || !running_;
                });
                batch.swap(queue_); // Swap out entire queue to reduce lock scope (交换出整个队列，减小锁的粒度)
            }

            // Batch write log entries (批量写入日志)
            for (const auto& entry : batch) {
                write_entry(entry);
            }

            // Check if periodic flush is needed (检查是否需要定期刷盘)
            if (std::chrono::steady_clock::now() >= next_flush) {
                const uint64_t total_drops = dropped_messages_.load(std::memory_order_relaxed);
                if (total_drops > last_reported_drops) {
                    const uint64_t new_drops = total_drops - last_reported_drops;
                    last_reported_drops = total_drops;
                    LogEntry drop_entry{
                        std::chrono::system_clock::now(),
                        LogLevel::Warn,
                        current_thread_id(),
                        "log queue overflow: " + std::to_string(new_drops) +
                            " messages dropped (total=" + std::to_string(total_drops) + ")"
                    };
                    write_entry(drop_entry);
                }
                flush_file();
                next_flush = std::chrono::steady_clock::now() + flush_interval;
            }
        }

        // Final flush before exit (退出前最后一次刷盘)
        flush_file();
    }

    // Check if queue has unprocessed entries (检查队列中是否有未处理的日志)
    bool has_pending_entries() const {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        return !queue_.empty();
    }

    // Format log entry as a string (将日志条目格式化为字符串)
    std::string format_line(const LogEntry& entry) const {
        std::string line = "[";
        line += format_timestamp(entry.timestamp);
        line += "][";
        line += log_level_name(entry.level);
        line += "][T";
        line += std::to_string(entry.thread_id);
        line += "] ";
        line += entry.message;
        return line;
    }

    // Write a single log entry to file and console (实际写入单条日志到文件和控制台)
    void write_entry(const LogEntry& entry) {
        rotate_if_needed(entry.timestamp); // Check if daily rotation needed (检查是否需要按天滚动文件)

        const std::string line = format_line(entry);
#ifdef _WIN32
        if (!write_console_utf8_line(line)) {
            std::fprintf(stdout, "%s\n", line.c_str()); // Fallback to regular output, compatible with redirect (退回到常规输出，兼容重定向场景)
        }
#else
        std::fprintf(stdout, "%s\n", line.c_str()); // Output to console (输出到控制台)
#endif

        if (fp_ != nullptr) {
            std::fprintf(fp_, "%s\n", line.c_str()); // Write to file (写入到文件)
            // Warn and Error levels are flushed immediately (警告和错误级别日志立即刷盘)
            if (entry.level != LogLevel::Info) {
                std::fflush(fp_);
            }
        }

        // Cache recent log entries for web interface display (将近期日志缓存，供 Web 界面展示)
        {
            std::lock_guard<std::mutex> lock(recent_mtx_);
            recent_entries_.push_back(RecentLogEntry{
                format_timestamp(entry.timestamp),
                log_level_name(entry.level),
                entry.thread_id,
                entry.message
            });
            // Keep recent log cache size within limit (保持近期日志缓存大小不超过限制)
            while (recent_entries_.size() > options_.recent_capacity) {
                recent_entries_.pop_front();
            }
        }

        written_messages_.fetch_add(1, std::memory_order_relaxed);
        if (entry.level == LogLevel::Error) error_count_.fetch_add(1, std::memory_order_relaxed);
        else if (entry.level == LogLevel::Warn) warn_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Rotate log file if date has changed (如果日期跨天，则滚动日志文件)
    void rotate_if_needed(std::chrono::system_clock::time_point tp) {
        const std::string token = format_date_token(tp);
        if (token == current_date_token_) return; // Date unchanged (日期未变)

        flush_file();
        close_log_file();
        open_log_file_for(tp);
    }

    // Open (or create) log file for the given time point (根据指定时间点打开或创建对应的日志文件)
    void open_log_file_for(std::chrono::system_clock::time_point tp) {
        namespace fs = std::filesystem;

        const std::string date_token = format_date_token(tp);
        const fs::path log_dir = fs::path(options_.log_dir);

        std::error_code ec;
        fs::create_directories(log_dir, ec); // Ensure directory exists (确保目录存在)

        const fs::path file_path = log_dir / (options_.file_prefix + "_" + date_token + ".log");
        const bool existed = fs::exists(file_path, ec);

        fp_ = std::fopen(file_path.string().c_str(), "ab"); // Open file in append mode (以追加模式打开文件)
        if (fp_ != nullptr && !existed) {
#ifdef _WIN32
            // On Windows, write UTF-8 BOM for new files to avoid mojibake in some editors
            // (Windows 下如果是新文件，写入 UTF-8 BOM，以防用某些编辑器打开乱码)
            const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
            std::fwrite(bom, 1, sizeof(bom), fp_);
#endif
        }

        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            current_date_token_ = date_token;
            current_file_ = file_path.string();
        }

        cleanup_expired_logs(tp); // Also clean up expired logs (顺便清理过期日志)
    }

    // Remove old log files that exceed retention days (清理超过保留天数的旧日志文件)
    void cleanup_expired_logs(std::chrono::system_clock::time_point now) {
        if (options_.retention_days <= 0) return; // If retention is 0, skip cleanup (如果设置为 0 则不清理)

        namespace fs = std::filesystem;
        // Compute cutoff time point (计算过期时间截断点)
        const auto cutoff = now - std::chrono::hours(24LL * options_.retention_days);
        const std::string cutoff_token = format_date_token(cutoff);
        const fs::path log_dir = fs::path(options_.log_dir);

        std::error_code ec;
        if (!fs::exists(log_dir, ec)) return;

        // Iterate log directory (遍历日志目录)
        for (const auto& entry : fs::directory_iterator(log_dir, ec)) {
            if (ec || !entry.is_regular_file()) continue;

            const std::string filename = entry.path().filename().string();
            const std::string prefix = options_.file_prefix + "_";

            // Filter out files not generated by this system (过滤非该系统生成的日志文件)
            if (filename.rfind(prefix, 0) != 0 || entry.path().extension() != ".log") {
                continue;
            }

            // Extract date part from filename (提取文件名中的日期部分)
            if (filename.size() < prefix.size() + 12) continue;
            const std::string date_token = filename.substr(prefix.size(), 8);
            // If date is before cutoff, remove file (如果日期小于截断点，删除文件)
            if (date_token < cutoff_token) {
                fs::remove(entry.path(), ec);
            }
        }
    }

    // Flush kernel buffer to disk (将内核缓冲区的日志刷入磁盘)
    void flush_file() {
        if (fp_ != nullptr) {
            std::fflush(fp_);
        }
    }

    // Close current log file (关闭当前日志文件)
    void close_log_file() {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
        current_file_.clear();
        current_date_token_.clear();
    }

    LoggerOptions options_; // Logger configuration (日志系统配置)

    mutable std::mutex queue_mtx_;           // Queue mutex (队列锁)
    std::condition_variable queue_cv_;       // Queue condition variable (队列条件变量)
    std::deque<LogEntry> queue_;             // Pending log queue (待处理日志队列)

    mutable std::mutex recent_mtx_;          // Recent log cache mutex (近期日志缓存锁)
    std::deque<RecentLogEntry> recent_entries_; // Recent log cache (近期日志缓存队列)

    mutable std::mutex state_mtx_;           // Internal state mutex (内部状态锁)
    std::atomic<uint64_t> dropped_messages_{0}; // Dropped message counter (丢弃消息计数器)
    std::atomic<uint64_t> written_messages_{0}; // Written message counter (写入消息计数器)
    std::atomic<uint64_t> error_count_{0};
    std::atomic<uint64_t> warn_count_{0};
    std::thread worker_thread_;              // Background consumer thread (后台消费线程)
    std::atomic<bool> running_{false};       // Running flag (运行标志位)
    FILE* fp_ = nullptr;                     // Log file handle (日志文件指针)
    std::string current_date_token_;         // Current log date token (当前日志的日期标记)
    std::string current_file_;               // Current log file path (当前日志文件路径)
};

#define ASYNC_LOG_INFO(fmt, ...)  hft::AsyncLogger::instance().log(hft::LogLevel::Info, fmt, ##__VA_ARGS__)
#define ASYNC_LOG_WARN(fmt, ...)  hft::AsyncLogger::instance().log(hft::LogLevel::Warn, fmt, ##__VA_ARGS__)
#define ASYNC_LOG_ERROR(fmt, ...) hft::AsyncLogger::instance().log(hft::LogLevel::Error, fmt, ##__VA_ARGS__)

} // namespace hft
