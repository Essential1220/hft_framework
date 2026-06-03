#pragma once
// ============================================
// logger.h - Lightweight log entry facade (轻量日志入口)
//
// Keeps Logger singleton as a facade for legacy code compatibility:
// (保留 Logger 单例作为外观层，方便旧代码继续调用)
//   Logger::instance().init(...)
//   LOG_INFO / LOG_WARN / LOG_ERROR
// ============================================

#include "common/async_logger.h"
#include <filesystem>
#include <string>
#include <thread>

namespace hft {

class Logger {
public:
    // Get Logger singleton instance (获取 Logger 单例实例)
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // Initialize logger with default or specified filename (初始化日志系统，使用默认或指定的文件名)
    void init(const std::string& log_file = "hft.log") {
        std::filesystem::path path(log_file);

        LoggerOptions options;
        if (path.has_parent_path()) {
            options.log_dir = path.parent_path().string(); // Set log directory (设置日志目录)
        }
        if (!path.stem().string().empty()) {
            options.file_prefix = path.stem().string(); // Set log file prefix (设置日志文件前缀)
        }

        AsyncLogger::instance().start(options); // Start async logging system (启动异步日志系统)
    }

    // Initialize logger with specified options (使用指定的选项初始化日志系统)
    void init(const LoggerOptions& options) {
        AsyncLogger::instance().start(options);
    }

    // Shutdown the logging system (关闭日志系统)
    void shutdown() {
        AsyncLogger::instance().stop();
    }

    // Get the async logger worker thread handle (获取异步日志的工作线程句柄)
    std::thread& worker_thread() {
        return AsyncLogger::instance().worker_thread();
    }

private:
    Logger() = default;
    ~Logger() {
        AsyncLogger::instance().stop(); // Ensure logger is stopped on destruction (析构时确保停止日志系统)
    }

    Logger(const Logger&) = delete; // Delete copy constructor (禁用拷贝构造)
    Logger& operator=(const Logger&) = delete; // Delete assignment operator (禁用赋值操作)
};

// Macro definitions: convenient logging at different levels (宏定义：方便记录不同级别的日志信息)
#define LOG_INFO(msg)  hft::AsyncLogger::instance().log_string(hft::LogLevel::Info, std::string(msg))
#define LOG_WARN(msg)  hft::AsyncLogger::instance().log_string(hft::LogLevel::Warn, std::string(msg))
#define LOG_ERROR(msg) hft::AsyncLogger::instance().log_string(hft::LogLevel::Error, std::string(msg))

} // namespace hft
