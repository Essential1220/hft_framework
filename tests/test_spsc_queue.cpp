// test_spsc_queue.cpp - SPSCQueue unit tests (SPSCQueue 单元测试)

#include "common/spsc_queue.h"
#include "common/event.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

// 从 test_main.cpp 引入 (imported from test_main.cpp)
extern void TEST_SUITE(const char*);
#define TEST_ASSERT(cond)                                                     \
    do { if (!(cond)) throw std::runtime_error(std::string("ASSERT: ") + #cond \
        + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)
#define TEST_ASSERT_EQ(a, b)                                                  \
    do { if ((a) != (b)) throw std::runtime_error(std::string("EQ: ") + #a +   \
        " != " #b + " " + __FILE__ + ":" + std::to_string(__LINE__)); } while(0)

using namespace hft;

void test_spsc_push_pop() {
    SPSCQueue<int, 16> q;
    int val;
    TEST_ASSERT(q.empty());

    TEST_ASSERT(q.push(42));
    TEST_ASSERT(!q.empty());
    TEST_ASSERT(q.pop(val));
    TEST_ASSERT_EQ(val, 42);
    TEST_ASSERT(q.empty());
}

void test_spsc_full_returns_false() {
    SPSCQueue<int, 4> q; // capacity = 3
    TEST_ASSERT(q.push(1));
    TEST_ASSERT(q.push(2));
    TEST_ASSERT(q.push(3));
    TEST_ASSERT(!q.push(4)); // full
}

void test_spsc_empty_returns_false() {
    SPSCQueue<int, 16> q;
    int val;
    TEST_ASSERT(!q.pop(val));
}

void test_spsc_size() {
    SPSCQueue<int, 16> q;
    TEST_ASSERT_EQ(q.size(), 0u);
    q.push(1);
    TEST_ASSERT_EQ(q.size(), 1u);
    q.push(2);
    q.push(3);
    TEST_ASSERT_EQ(q.size(), 3u);
    int val;
    q.pop(val);
    TEST_ASSERT_EQ(q.size(), 2u);
}

void test_spsc_capacity() {
    SPSCQueue<int, 16> q;
    TEST_ASSERT_EQ(q.capacity(), 15u);
    SPSCQueue<int, 1024> q2;
    TEST_ASSERT_EQ(q2.capacity(), 1023u);
}

void test_spsc_drop_count() {
    SPSCQueue<int, 4> q;
    TEST_ASSERT_EQ(q.drop_count(), 0u);
    q.push(1);
    q.push(2);
    q.push(3);
    TEST_ASSERT(!q.push(4)); // full, drop
    TEST_ASSERT_EQ(q.drop_count(), 1u);
    TEST_ASSERT(!q.push(5)); // full, drop
    TEST_ASSERT_EQ(q.drop_count(), 2u);
}

void test_spsc_stress_million() {
    constexpr size_t N = 1000000;
    SPSCQueue<size_t, 4096> q;

    auto start = std::chrono::steady_clock::now();

    size_t pushed = 0, popped = 0;
    for (size_t i = 0; i < N; ++i) {
        if (q.push(i)) ++pushed;
    }
    size_t val;
    while (q.pop(val)) ++popped;

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();

    TEST_ASSERT_EQ(pushed, popped);
    TEST_ASSERT(pushed > 0);
    printf("    [info] pushed=%zu popped=%zu elapsed=%.2f ms (%.0f ops/ms)\n",
           pushed, popped, ms, (pushed + popped) / ms);
}
