#include "app/app_runtime.h"
#include "common/config.h"
#include "common/huge_page.h"
#include "common/logger.h"
#include "common/string_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace hft {
namespace {

enum class RunMode { Interactive, Service };

std::atomic<bool> g_should_exit{false};

void startup_trace(const char* step) {
    if (std::getenv("HFT_STARTUP_TRACE") == nullptr) return;
    std::ofstream ofs("startup_trace.log", std::ios::app);
    ofs << step << "\n";
}

void request_exit() {
    g_should_exit.store(true, std::memory_order_relaxed);
}


std::string get_user_data_dir() {
#ifdef _WIN32
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data && local_app_data[0] != '\0') {
        return (std::filesystem::path(local_app_data) / "HFTFramework").string();
    }
    const char* user_profile = std::getenv("USERPROFILE");
    if (user_profile && user_profile[0] != '\0') {
        return (std::filesystem::path(user_profile) / "AppData" / "Local" / "HFTFramework").string();
    }
    return (std::filesystem::path(".") / "HFTFramework").string();
#else
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return (std::filesystem::path(home) / ".hft_framework").string();
    }
    return (std::filesystem::path(".") / ".hft_framework").string();
#endif
}

std::string resolve_config_path(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--config=", 0) == 0) {
            const std::string value = trim_copy(arg.substr(9));
            if (!value.empty()) return value;
        }
        if (arg == "--config" && i + 1 < argc) {
            const std::string value = trim_copy(argv[i + 1]);
            if (!value.empty()) return value;
        }
    }
    return (std::filesystem::path(get_user_data_dir()) / "config.ini").string();
}

RunMode parse_run_mode(std::string text, RunMode fallback) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (text == "interactive" || text == "console") return RunMode::Interactive;
    if (text == "service" || text == "daemon") return RunMode::Service;
    return fallback;
}

RunMode resolve_run_mode(int argc, char** argv, const Config& config) {
#ifdef _WIN32
    RunMode fallback = RunMode::Interactive;
#else
    RunMode fallback = RunMode::Service;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--interactive") return RunMode::Interactive;
        if (arg == "--service") return RunMode::Service;
        if (arg.rfind("--mode=", 0) == 0) return parse_run_mode(arg.substr(7), fallback);
    }
    const std::string configured = trim_copy(config.get_string("Runtime", "RunMode", ""));
    return configured.empty() ? fallback : parse_run_mode(configured, fallback);
}

int resolve_startup_delay_ms(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        try {
            if (arg.rfind("--startup-delay-ms=", 0) == 0) return std::max(0, std::stoi(arg.substr(19)));
            if (arg == "--startup-delay-ms" && i + 1 < argc) return std::max(0, std::stoi(argv[i + 1]));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

void install_signal_handlers() {
    std::signal(SIGINT, [](int) { request_exit(); });
    std::signal(SIGTERM, [](int) { request_exit(); });
#ifdef _WIN32
    SetConsoleCtrlHandler(
        [](DWORD type) -> BOOL {
            if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
                type == CTRL_SHUTDOWN_EVENT) {
                request_exit();
                return TRUE;
            }
            return FALSE;
        },
        TRUE);
#endif
}

void set_thread_affinity(std::thread& thread, int cpu_core) {
    if (cpu_core < 0 || !thread.joinable()) return;
#ifdef _WIN32
    const DWORD_PTR mask = DWORD_PTR{1} << static_cast<DWORD_PTR>(cpu_core);
    SetThreadAffinityMask(static_cast<HANDLE>(thread.native_handle()), mask);
#else
    (void)thread;
    (void)cpu_core;
#endif
}

// 预锁定当前线程的工作集内存，防止页面换入换出导致的延迟抖动
void lock_current_thread_memory() {
#ifdef _WIN32
    // VirtualLock requires OS privileges and can block startup on some desktop
    // environments. Keep it opt-in; startup reliability is more important than
    // this small latency optimization.
    const char* enabled = std::getenv("HFT_LOCK_STACK");
    if (!enabled || std::string(enabled) != "1") return;

    volatile char stack_probe[64 * 1024]{};
    stack_probe[0] = 1;
    stack_probe[sizeof(stack_probe) - 1] = 1;

    // 先提升进程权限以允许 VirtualLock
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid)) {
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        }
        CloseHandle(hToken);
    }
    // 锁定当前线程已提交的栈页；失败不影响启动。
    if (!VirtualLock((LPVOID)stack_probe, sizeof(stack_probe))) {
        LOG_WARN("VirtualLock failed, error=" + std::to_string(GetLastError()));
    }
#endif
}

// 设置线程为高优先级，减少 OS 调度延迟
void set_thread_high_priority(std::thread& thread) {
#ifdef _WIN32
    if (!thread.joinable()) return;
    SetThreadPriority(static_cast<HANDLE>(thread.native_handle()), THREAD_PRIORITY_HIGHEST);
#else
    (void)thread;
#endif
}

// 设置进程为高优先级，减少内核调度延迟
void set_process_realtime_priority() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
}

// 将 CTP SDK 内部线程绑定到指定 CPU 核心
// CTP API 在 Init() 时创建内部线程，这里通过线程快照枚举并绑定
void pin_process_threads_to_core(int exclude_tid_main, int target_core) {
#ifdef _WIN32
    if (target_core < 0) return;
    const DWORD_PTR mask = DWORD_PTR{1} << static_cast<DWORD_PTR>(target_core);
    const DWORD current_pid = GetCurrentProcessId();
    const DWORD current_tid = GetCurrentThreadId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == current_pid &&
                te.th32ThreadID != current_tid &&
                static_cast<int>(te.th32ThreadID) != exclude_tid_main) {
                HANDLE hThread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                                            FALSE, te.th32ThreadID);
                if (hThread) {
                    SetThreadAffinityMask(hThread, mask);
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
#else
    (void)exclude_tid_main;
    (void)target_core;
#endif
}

void run_interactive_loop() {
    LOG_INFO("interactive mode running, input q/quit/exit to stop");
    std::string line;
    while (!g_should_exit.load(std::memory_order_relaxed)) {
        if (!std::getline(std::cin, line)) {
            if (std::cin.eof()) break;
            std::cin.clear();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        line = trim_copy(line);
        if (line == "q" || line == "quit" || line == "exit") {
            request_exit();
        }
    }
}

void run_service_loop() {
    LOG_INFO("service mode running, waiting for shutdown signal");
    while (!g_should_exit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

LoggerOptions build_logger_options(const Config& config) {
    LoggerOptions options;
    options.log_dir = config.get_string("Log", "Directory", options.log_dir);
    options.file_prefix = config.get_string("Log", "FilePrefix", options.file_prefix);
    options.queue_capacity = static_cast<size_t>(std::max(1, config.get_int("Log", "QueueCapacity", static_cast<int>(options.queue_capacity))));
    options.recent_capacity = static_cast<size_t>(std::max(1, config.get_int("Log", "RecentBufferSize", static_cast<int>(options.recent_capacity))));
    options.flush_interval_ms = std::max(100, config.get_int("Log", "FlushIntervalMs", options.flush_interval_ms));
    options.retention_days = std::max(1, config.get_int("Log", "RetentionDays", options.retention_days));
    options.min_level = parse_log_level(config.get_string("Log", "Level", "INFO"));
    return options;
}

// 首次运行时自动创建最小 config.ini，避免因缺失配置文件导致启动失败
void ensure_default_config(const std::string& path) {
    if (std::filesystem::exists(path)) return;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << "; HFT Framework - auto-generated default config\n";
    ofs << "; Edit this file to configure the HFT framework.\n\n";
    ofs << "[CTP]\n";
    ofs << "BrokerID = \n";
    ofs << "UserID = \n";
    ofs << "Password = \n";
    ofs << "AuthCode = \n";
    ofs << "AppID = \n";
    ofs << "MdAddress = \n";
    ofs << "TdAddress = \n\n";
    ofs << "[Risk]\n";
    ofs << "MaxOrderSize = 5\n";
    ofs << "MaxNetPosition = 10\n";
    ofs << "MaxDailyLoss = 5000\n\n";
    ofs << "[Log]\n";
    ofs << "Level = INFO\n";
    ofs << "Directory = " << (std::filesystem::path(get_user_data_dir()) / "logs").string() << "\n";
    ofs.close();
    LOG_INFO("auto-created default config.ini at " + path);
}

} // namespace
} // namespace hft

int main(int argc, char** argv) {
    using namespace hft;

    install_signal_handlers();
    startup_trace("after_signal_handlers");
    lock_current_thread_memory();  // 预锁定主线程栈，减少页面换入换出抖动
    startup_trace("after_lock_memory");
    const std::string config_path = std::filesystem::absolute(resolve_config_path(argc, argv)).string();
    startup_trace("after_config_path");

    ensure_default_config(config_path);
    startup_trace("after_ensure_config");
    Config boot_config;
    const bool boot_config_loaded = boot_config.load(config_path);
    startup_trace("after_boot_config_load");
    Logger::instance().init(build_logger_options(boot_config));
    startup_trace("after_logger_init");

    LOG_INFO("========================================");
    LOG_INFO("  HFT Framework start");
    LOG_INFO("========================================");
    if (!boot_config_loaded) {
        LOG_WARN("boot config not loaded, using defaults: " + config_path);
    }

    const RunMode run_mode = resolve_run_mode(argc, argv, boot_config);
    LOG_INFO(std::string("run mode: ") + (run_mode == RunMode::Interactive ? "interactive" : "service"));

    // 进程级优化：实时优先级 + 关闭动态优先级提升
    const char* realtime_enabled = std::getenv("HFT_REALTIME_PRIORITY");
    if (boot_config.get_int("Performance", "RealtimePriority", 1) > 0 &&
        realtime_enabled && std::string(realtime_enabled) == "1") {
        set_process_realtime_priority();
        LOG_INFO("process set to HIGH_PRIORITY_CLASS");
    } else if (boot_config.get_int("Performance", "RealtimePriority", 1) > 0) {
        LOG_INFO("process realtime priority skipped; set HFT_REALTIME_PRIORITY=1 to enable");
    }

    const int logger_cpu = boot_config.get_int("Performance", "LoggerCpuCore", -1);
    if (logger_cpu >= 0) {
        set_thread_affinity(Logger::instance().worker_thread(), logger_cpu);
    }

    if (hft::enable_lock_memory_privilege()) {
        LOG_INFO("SeLockMemoryPrivilege enabled — large pages available");
    }

    AppRuntime runtime;
    startup_trace("before_runtime_initialize");
    if (!runtime.initialize(config_path, []() { request_exit(); })) {
        Logger::instance().shutdown();
        return -1;
    }

    const Config& config = runtime.config();
    startup_trace("after_runtime_initialize");
    const int startup_delay_ms = resolve_startup_delay_ms(argc, argv);

    if (!runtime.start(startup_delay_ms)) {
        LOG_ERROR("runtime start failed");
        runtime.stop();
        Logger::instance().shutdown();
        return -1;
    }
    startup_trace("after_runtime_start");

#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    const int engine_cpu = config.get_int("Performance", "EngineCpuCore", -1);
    if (runtime.engine()) {
        auto& ct = runtime.engine()->get_consumer_thread();
        if (engine_cpu >= 0) {
            set_thread_affinity(ct, engine_cpu);
            LOG_INFO("consumer thread bound to CPU core " + std::to_string(engine_cpu));
        }
        if (config.get_int("Performance", "EngineHighPriority", 1) > 0) {
            set_thread_high_priority(ct);
            LOG_INFO("consumer thread set to high priority");
        }
#ifdef _WIN32
        SetThreadPriorityBoost(static_cast<HANDLE>(ct.native_handle()), TRUE);
#endif
    }

#ifdef _WIN32
    if (!SetProcessWorkingSetSizeEx(GetCurrentProcess(),
            200ULL * 1024 * 1024, 400ULL * 1024 * 1024,
            QUOTA_LIMITS_HARDWS_MIN_ENABLE)) {
        LOG_WARN("SetProcessWorkingSetSizeEx failed, error=" + std::to_string(GetLastError()));
    }
#endif

    // 将 CTP SDK 内部线程绑定到指定 CPU 核心，减少 OS 调度抖动
    const int ctp_cpu = config.get_int("Performance", "CtpThreadCpuCore", -1);
    if (ctp_cpu >= 0) {
#ifdef _WIN32
        pin_process_threads_to_core(static_cast<int>(GetCurrentThreadId()), ctp_cpu);
#endif
        LOG_INFO("CTP SDK threads pinned to CPU core " + std::to_string(ctp_cpu));
    }

    if (run_mode == RunMode::Interactive) {
        run_interactive_loop();
    } else {
        run_service_loop();
    }

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    runtime.stop();
    Logger::instance().shutdown();
    return 0;
}
