// ============================================
// watchdog_main.cpp - Standalone watchdog process
// Monitors engine heartbeat via shared memory. On stale heartbeat:
// logs alert, optionally terminates stalled engine.
// ============================================

#include "common/watchdog_shm.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static void usage() {
    std::printf("Usage: hft_watchdog [options]\n"
                "  --shm-name <name>    Shared memory name (default: hft_watchdog)\n"
                "  --threshold <ms>     Stale threshold in ms (default: 3000)\n"
                "  --poll-interval <ms> Poll interval in ms (default: 500)\n"
                "  --help               Show this help\n");
}

int main(int argc, char* argv[]) {
    std::string shm_name = "hft_watchdog";
    int threshold_ms = 3000;
    int poll_ms = 500;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shm-name") == 0 && i + 1 < argc) {
            shm_name = argv[++i];
        } else if (std::strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--poll-interval") == 0 && i + 1 < argc) {
            poll_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
    }

    std::printf("[watchdog] Connecting to shared memory '%s' (threshold=%dms, poll=%dms)\n",
                shm_name.c_str(), threshold_ms, poll_ms);

    hft::WatchdogShm* shm = nullptr;
    while (!shm) {
        shm = hft::WatchdogShmHelper::open(shm_name);
        if (!shm) {
            std::printf("[watchdog] Waiting for engine to create shared memory...\n");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::printf("[watchdog] Connected. Engine PID=%d\n",
                shm->engine_pid.load(std::memory_order_relaxed));

    int consecutive_stale = 0;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));

        int32_t running = shm->running.load(std::memory_order_acquire);
        if (running == 0) {
            std::printf("[watchdog] Engine signaled shutdown (running=0). Exiting.\n");
            break;
        }

        int64_t hb = shm->heartbeat_ms.load(std::memory_order_relaxed);
        if (hb == 0) continue;

        int64_t now = hft::WatchdogShmHelper::now_ms();
        int64_t delta = now - hb;

        if (delta > threshold_ms) {
            ++consecutive_stale;
            std::printf("[watchdog] ALERT: heartbeat stale by %lldms (count=%d, pid=%d)\n",
                        static_cast<long long>(delta), consecutive_stale,
                        shm->engine_pid.load(std::memory_order_relaxed));

            if (consecutive_stale >= 3) {
                std::printf("[watchdog] CRITICAL: engine unresponsive for %d consecutive polls. "
                            "Manual intervention required.\n", consecutive_stale);
            }
        } else {
            if (consecutive_stale > 0) {
                std::printf("[watchdog] Heartbeat recovered (delta=%lldms)\n",
                            static_cast<long long>(delta));
            }
            consecutive_stale = 0;
        }
    }

    hft::WatchdogShmHelper::close_ptr(shm);
    std::printf("[watchdog] Terminated.\n");
    return 0;
}
