#pragma once
// ============================================
// hft_api.h - C API for HFT Trading Engine (HFT 交易引擎 C 语言接口)
// Opaque handle + extern "C" for FFI (Go, Rust, C#, Java JNI, Python ctypes)
// ============================================

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef HFT_CAPI_EXPORTS
#define HFT_API __declspec(dllexport)
#else
#define HFT_API __declspec(dllimport)
#endif
#else
#define HFT_API __attribute__((visibility("default")))
#endif

typedef struct HftEngine* HftEngineHandle;

// ---- Lifecycle (生命周期) ----
HFT_API HftEngineHandle hft_engine_create(void);
HFT_API int  hft_engine_init(HftEngineHandle h, const char* config_path);
HFT_API int  hft_engine_start(HftEngineHandle h);
HFT_API void hft_engine_stop(HftEngineHandle h);
HFT_API void hft_engine_destroy(HftEngineHandle h);

// ---- Trading (交易操作) ----
HFT_API int  hft_send_order(HftEngineHandle h, const char* instrument,
                            int direction, int offset, double price,
                            int volume, char* out_ref, int ref_len);
HFT_API int  hft_cancel_order(HftEngineHandle h, const char* order_ref);
HFT_API int  hft_cancel_all(HftEngineHandle h, const char* account_id);

// ---- Query — returns heap-allocated JSON, caller must hft_free_string (查询，返回堆分配 JSON) ----
HFT_API char* hft_get_account(HftEngineHandle h, const char* account_id);
HFT_API char* hft_get_positions(HftEngineHandle h, const char* account_id);
HFT_API char* hft_get_active_orders(HftEngineHandle h, const char* account_id);
HFT_API char* hft_get_last_tick(HftEngineHandle h, const char* instrument);
HFT_API char* hft_get_latency(HftEngineHandle h);
HFT_API char* hft_get_pnl_curve(HftEngineHandle h, int limit);
HFT_API char* hft_get_strategy_snapshots(HftEngineHandle h);
HFT_API char* hft_get_account_snapshots(HftEngineHandle h);
HFT_API char* hft_get_risk_snapshot(HftEngineHandle h, const char* account_id);
HFT_API char* hft_get_recent_alerts(HftEngineHandle h, int limit);
HFT_API char* hft_get_recent_orders(HftEngineHandle h, const char* account_id, int limit);
HFT_API char* hft_get_recent_trades(HftEngineHandle h, const char* account_id, int limit);

// ---- State (状态) ----
HFT_API int  hft_is_running(HftEngineHandle h);
HFT_API int  hft_is_trading_ready(HftEngineHandle h);
HFT_API int  hft_get_risk_mode(HftEngineHandle h);
HFT_API void hft_set_risk_mode(HftEngineHandle h, int mode, const char* reason);

// ---- Memory (内存管理) ----
HFT_API void hft_free_string(char* s);

#ifdef __cplusplus
}
#endif
