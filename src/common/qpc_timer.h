#pragma once
// ============================================
// qpc_timer.h - Windows QPC sub-microsecond timer (Windows QPC 亚微秒计时器)
// Replaces std::chrono for hot path measurement (替代 std::chrono 用于热路径测量)
// ============================================

#include <cstdint>
#include <algorithm>
#include <vector>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <time.h>
#endif

namespace hft {

// Sub-microsecond precision timer using QPC (Windows) or clock_gettime (Linux)
// (基于 QPC (Windows) 或 clock_gettime (Linux) 的亚微秒精度计时器)
class QPCTimer {
public:
    void start() {
#ifdef _WIN32
        QueryPerformanceCounter(&t0_);
#else
        clock_gettime(CLOCK_MONOTONIC, &t0_);
#endif
    }

    void stop() {
#ifdef _WIN32
        QueryPerformanceCounter(&t1_);
#else
        clock_gettime(CLOCK_MONOTONIC, &t1_);
#endif
    }

    int64_t elapsed_ns() const {
#ifdef _WIN32
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        int64_t ticks = t1_.QuadPart - t0_.QuadPart;
        return (ticks * 1000000000LL) / freq.QuadPart;
#else
        int64_t sec = t1_.tv_sec - t0_.tv_sec;
        int64_t nsec = t1_.tv_nsec - t0_.tv_nsec;
        return sec * 1000000000LL + nsec;
#endif
    }

    int64_t elapsed_us() const {
        return elapsed_ns() / 1000;
    }

    double elapsed_us_f() const {
        return static_cast<double>(elapsed_ns()) / 1000.0;
    }

private:
#ifdef _WIN32
    LARGE_INTEGER t0_{};
    LARGE_INTEGER t1_{};
#else
    struct timespec t0_{};
    struct timespec t1_{};
#endif
};

// Collect latency samples and compute percentiles (收集延迟样本并计算分位数)
class QPCLatencyStats {
public:
    explicit QPCLatencyStats(size_t capacity = 10000) {
        samples_ns_.reserve(capacity);
    }

    void record(int64_t ns) {
        samples_ns_.push_back(ns);
    }

    void record_elapsed(const QPCTimer& timer) {
        record(timer.elapsed_ns());
    }

    size_t count() const { return samples_ns_.size(); }

    void reset() { samples_ns_.clear(); }

    struct Percentiles {
        double p50_us = 0;
        double p95_us = 0;
        double p99_us = 0;
        double p999_us = 0;
        double max_us = 0;
        double min_us = 0;
        double avg_us = 0;
    };

    Percentiles compute() const {
        Percentiles r{};
        if (samples_ns_.empty()) return r;

        std::vector<int64_t> sorted = samples_ns_;
        std::sort(sorted.begin(), sorted.end());
        size_t n = sorted.size();

        r.min_us = sorted[0] / 1000.0;
        r.p50_us = sorted[n * 50 / 100] / 1000.0;
        r.p95_us = sorted[n * 95 / 100] / 1000.0;
        r.p99_us = sorted[n * 99 / 100] / 1000.0;
        r.p999_us = sorted[static_cast<size_t>(n * 99.9 / 100)] / 1000.0;
        r.max_us = sorted[n - 1] / 1000.0;

        int64_t sum = 0;
        for (auto v : samples_ns_) sum += v;
        r.avg_us = (sum / static_cast<int64_t>(n)) / 1000.0;

        return r;
    }

    void print(const char* label) const {
        auto p = compute();
        std::printf("    [%s] n=%zu  min=%.2f us  p50=%.2f us  p95=%.2f us  p99=%.2f us  p99.9=%.2f us  max=%.2f us  avg=%.2f us\n",
                    label, samples_ns_.size(),
                    p.min_us, p.p50_us, p.p95_us, p.p99_us, p.p999_us, p.max_us, p.avg_us);
    }

private:
    std::vector<int64_t> samples_ns_;
};

} // namespace hft
