#pragma once
// ============================================
// latency.h - High-precision latency measurement tools (高精度延迟测量工具)
// LatencyTimer: single measurement (单次计时)
// LatencyStats: cumulative stats min/max/avg/p99 (累计统计 min/max/avg/p99)
// ============================================

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <vector>

namespace hft {

class LatencyTimer {
public:
    // Start timing (开始计时)
    void start() { t0_ = Clock::now(); }
    
    // Stop timing (停止计时)
    void stop()  { t1_ = Clock::now(); }

    // Get elapsed time from start to stop in nanoseconds (获取从 start 到 stop 经过的时间，纳秒)
    int64_t elapsed_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t1_ - t0_).count();
    }
    
    // Get elapsed time from start to stop in microseconds (获取从 start 到 stop 经过的时间，微秒)
    int64_t elapsed_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(t1_ - t0_).count();
    }

private:
    using Clock = std::chrono::high_resolution_clock; // High-resolution clock (使用高精度时钟)
    Clock::time_point t0_{}; // Start time (记录开始时间)
    Clock::time_point t1_{}; // End time (记录结束时间)
};

// Collect latency samples and print stats every report_interval samples (min/max/avg/p99)
// (收集延迟样本，并每隔 report_interval 个样本打印统计信息，包含 min/max/avg/p99)
class LatencyStats {
public:
    // Constructor, set report interval (构造函数，设置汇报周期)
    explicit LatencyStats(int report_interval = 1000)
        : report_interval_(report_interval) {
        samples_.reserve(report_interval); // Pre-allocate sample storage for performance (预分配样本存储空间以提高性能)
    }

    // Record a single latency sample in nanoseconds (记录单次延迟数据，纳秒)
    void record(int64_t ns) {
        // Update min and max (更新最小值和最大值)
        if (ns < min_) min_ = ns;
        if (ns > max_) max_ = ns;

        sum_ += ns;   // Accumulate total (累加总耗时)
        ++count_;     // Increment sample count (增加样本数)
        samples_.push_back(ns); // Save current sample (保存当前样本)

        // When report interval reached, print stats and reset (达到汇报周期后，打印统计信息并重置状态)
        if (static_cast<int>(samples_.size()) >= report_interval_) {
            print_and_reset();
        }
    }

private:
    // Compute stats and print, then reset for next cycle (计算统计数据并打印，随后重置内部状态)
    void print_and_reset() {
        // Sort samples to compute P99 percentile (对样本进行排序，计算 P99 分位数)
        std::sort(samples_.begin(), samples_.end());
        size_t n = samples_.size();

        // Compute P99 index (计算 P99 对应的索引)
        size_t p99_idx = static_cast<size_t>(n * 0.99);
        if (p99_idx >= n) p99_idx = n - 1; // Prevent out-of-bounds (防止越界)

        int64_t p99 = samples_[p99_idx]; // Get P99 value (获取 P99 值)
        int64_t avg = (count_ > 0) ? (sum_ / count_) : 0; // Compute average (计算平均值)

        // Format and print stats (格式化输出统计信息)
        std::printf("[LatencyStats] ticks=%lld  avg=%lld ns  min=%lld ns  max=%lld ns  p99=%lld ns  (avg=%.1f us)\n",
                    (long long)count_, (long long)avg,
                    (long long)min_, (long long)max_, (long long)p99,
                    avg / 1000.0);
        std::fflush(stdout); // Force flush output buffer (强制刷新输出缓冲区)

        // Reset internal state (重置内部状态)
        samples_.clear();
        samples_.reserve(report_interval_); // Re-pre-allocate space (重新预分配空间)
        min_ = INT64_MAX; // Reset min to maximum possible (重置最小值为最大可能值)
        max_ = 0;         // Reset max to 0 (重置最大值为 0)
        sum_ = 0;         // Reset sum (重置总和)
        count_ = 0;       // Reset counter (重置计数器)
    }

    int report_interval_;           // Report period (how many samples before reporting) (统计周期，收集多少个样本后汇报一次)
    int64_t min_ = INT64_MAX;       // Min latency in this period (本周期内的最小延迟)
    int64_t max_ = 0;               // Max latency in this period (本周期内的最大延迟)
    int64_t sum_ = 0;               // Total latency sum in this period (本周期内的总延迟和)
    int64_t count_ = 0;             // Sample count in this period (本周期内的样本总数)
    std::vector<int64_t> samples_;  // All latency samples in this period (保存本周期内的所有延迟样本)
};

} // namespace hft
