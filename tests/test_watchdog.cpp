// ============================================
// test_watchdog.cpp - Watchdog shared memory tests
// ============================================

#include "common/watchdog_shm.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#define TEST_ASSERT(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            throw std::runtime_error(std::string("ASSERT FAILED: ") + #cond +  \
                                     " at " + __FILE__ + ":" +                 \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_EQ(a, b)                                                  \
    do {                                                                       \
        if ((a) != (b)) {                                                      \
            throw std::runtime_error(std::string("ASSERT_EQ FAILED: ") +       \
                                     #a " != " #b " at " + __FILE__ + ":" +   \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

static const char* kTestShmName = "hft_test_watchdog_shm";

void test_watchdog_shm_create_and_read() {
    auto* shm = hft::WatchdogShmHelper::create(kTestShmName);
    TEST_ASSERT(shm != nullptr);
    TEST_ASSERT(std::memcmp(shm->magic, hft::kWatchdogMagic, 8) == 0);
    TEST_ASSERT_EQ(shm->running.load(), int32_t(1));
    TEST_ASSERT_EQ(shm->heartbeat_ms.load(), int64_t(0));

    shm->heartbeat_ms.store(12345, std::memory_order_relaxed);
    shm->engine_pid.store(9999, std::memory_order_relaxed);
    TEST_ASSERT_EQ(shm->heartbeat_ms.load(), int64_t(12345));
    TEST_ASSERT_EQ(shm->engine_pid.load(), int32_t(9999));

    shm->running.store(0, std::memory_order_release);
    hft::WatchdogShmHelper::close_ptr(shm);
    hft::WatchdogShmHelper::destroy(kTestShmName);
}

void test_watchdog_shm_cross_view() {
    auto* writer = hft::WatchdogShmHelper::create(kTestShmName);
    TEST_ASSERT(writer != nullptr);

    auto* reader = hft::WatchdogShmHelper::open(kTestShmName);
    TEST_ASSERT(reader != nullptr);

    int64_t ts = hft::WatchdogShmHelper::now_ms();
    writer->heartbeat_ms.store(ts, std::memory_order_release);
    writer->engine_pid.store(42, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    int64_t read_ts = reader->heartbeat_ms.load(std::memory_order_acquire);
    TEST_ASSERT_EQ(read_ts, ts);
    TEST_ASSERT_EQ(reader->engine_pid.load(std::memory_order_acquire), int32_t(42));

    writer->running.store(0, std::memory_order_release);
    hft::WatchdogShmHelper::close_ptr(reader);
    hft::WatchdogShmHelper::close_ptr(writer);
    hft::WatchdogShmHelper::destroy(kTestShmName);
}

void test_watchdog_shm_stale_detection() {
    auto* shm = hft::WatchdogShmHelper::create(kTestShmName);
    TEST_ASSERT(shm != nullptr);

    int64_t old_ts = hft::WatchdogShmHelper::now_ms() - 5000;
    shm->heartbeat_ms.store(old_ts, std::memory_order_relaxed);

    int64_t now = hft::WatchdogShmHelper::now_ms();
    int64_t delta = now - shm->heartbeat_ms.load(std::memory_order_relaxed);
    TEST_ASSERT(delta >= 4900);

    shm->running.store(0, std::memory_order_release);
    hft::WatchdogShmHelper::close_ptr(shm);
    hft::WatchdogShmHelper::destroy(kTestShmName);
}
