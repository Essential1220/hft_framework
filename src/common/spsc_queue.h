#pragma once
// ============================================
// spsc_queue.h - Lock-free SPSC (Single Producer Single Consumer) queue (无锁单生产者单消费者队列)
// Cache line aligned, zero syscall (cache line 对齐，零系统调用)
// ============================================

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hft {

template <typename T, size_t N>
class SPSCQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2"); // Capacity must be a power of 2 (容量必须是 2 的幂)
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable"); // Element must be trivially copyable (元素必须是可平凡复制的)

public:
    SPSCQueue() : head_(0), tail_(0) {}

    // Producer: push element, returns false if full (生产者调用：将元素压入队列。如果队列已满则返回 false)
    bool push(const T& item) {
        const size_t h = head_.load(std::memory_order_relaxed); // Load head position (获取当前队头位置)
        const size_t next = (h + 1) & kMask; // Compute next head position (计算下一个队头位置)
        if (next == tail_.load(std::memory_order_acquire)) { // Check if queue is full (检查是否与队尾重合，队列已满)
            drop_count_.fetch_add(1, std::memory_order_relaxed); // Increment drop count (记录丢弃次数)
            return false;
        }
        buf_[h] = item; // Store item (存入元素)
        head_.store(next, std::memory_order_release); // Update head position (更新队头位置)
        return true;
    }

    // Consumer: pop element, returns false if empty (消费者调用：从队列中弹出元素。如果队列为空则返回 false)
    bool pop(T& item) {
        const size_t t = tail_.load(std::memory_order_relaxed); // Load tail position (获取当前队尾位置)
        if (t == head_.load(std::memory_order_acquire)) { // Check if queue is empty (检查是否与队头重合，队列为空)
            return false;
        }
        item = buf_[t]; // Take item (取出元素)
        tail_.store((t + 1) & kMask, std::memory_order_release); // Update tail position (更新队尾位置)
        return true;
    }

    // Check if queue is empty (检查队列是否为空)
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    // Current number of pending elements in queue (当前队列中待处理元素数量)
    size_t size() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & kMask;
    }

    // Get queue capacity (actual capacity is N-1, one slot reserved to distinguish full/empty)
    // (获取队列容量，实际容量为 N - 1，因为要留一个空位区分满和空)
    size_t capacity() const { return N - 1; }

    // Cumulative push failure (drop) count (累计 push 失败丢弃次数)
    size_t drop_count() const { return drop_count_.load(std::memory_order_relaxed); }

private:
    static constexpr size_t kMask = N - 1; // Mask for fast modulo operation (掩码，用于快速取模运算)

    // alignas(64) ensures cache line alignment to avoid false sharing
    // (alignas(64) 确保变量对齐到缓存行边界，避免伪共享)
    alignas(64) std::atomic<size_t> head_; // Head pointer, written by producer (队头指针，生产者写入)
    alignas(64) std::atomic<size_t> tail_; // Tail pointer, read by consumer (队尾指针，消费者读取)
    alignas(64) T buf_[N];                 // Ring buffer (环形缓冲区)
    alignas(64) std::atomic<size_t> drop_count_{0}; // Cumulative drop count (累计丢弃次数)
};

} // namespace hft
