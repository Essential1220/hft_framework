#pragma once
// ============================================
// i_md_gateway.h - Market data gateway abstract interface (行情网关抽象接口)
//
// Defines the contract that all market data gateways (CTP MD, QDP MD, etc.) must fulfill.
// 定义所有行情网关（CTP 行情、QDP 行情等）必须实现的接口契约。
// ============================================

#include "common/config.h"

#include <functional>
#include <string>
#include <vector>

namespace hft {

class TradingEngine;

// Market data gateway connection status (行情网关连接状态)
enum class MdGatewayStatus { Disconnected, Connecting, Connected, LoggedIn };

// Market data gateway interface: connect, login, subscribe/unsubscribe instruments, receive ticks
// 行情网关接口：连接、登录、订阅/取消订阅合约、接收行情 Tick (行情网关接口)
class IMdGateway {
public:
    virtual ~IMdGateway() = default;

    // Initialize gateway with config, section name, and engine pointer
    // 初始化网关：传入配置、配置段名、引擎指针 (初始化行情网关)
    virtual void init(const Config& config, const std::string& section, TradingEngine* engine) = 0;

    // Subscribe to market data for given instruments (订阅合约行情 / 订阅行情)
    virtual void subscribe(const std::vector<std::string>& instruments) = 0;
    // Unsubscribe from market data for given instruments (取消订阅合约行情 / 取消订阅)
    virtual void unsubscribe(const std::vector<std::string>& instruments) = 0;

    // Incremental subscription: only subscribe new instruments without resetting existing set;
    // 增量订阅：只下发本次合约请求，不重置已订阅集合；订阅成功后追加到内部记录，
    // appended instruments are included in auto re-subscription after reconnect.
    // reconnect 自动重订时仍会包含这批合约。默认实现退化为全量 subscribe 以兼容自定义网关。
    // Default falls back to full subscribe for compatibility with custom gateways.
    virtual void subscribe_append(const std::vector<std::string>& instruments) {
        subscribe(instruments);
    }

    // Stop the gateway and release resources
    // 停止网关，释放资源 (停止行情网关)
    virtual void stop() = 0;

    virtual bool is_logged_in() const = 0;
    // Block until login completes or timeout (wait for login / 等待登录完成)
    virtual bool wait_for_login(int timeout_sec = 10) = 0;

    // Get current gateway connection status (获取当前网关连接状态)
    virtual MdGatewayStatus status() const = 0;

    // Callback invoked when gateway status changes (网关状态变化回调)
    using StatusCallback = std::function<void(MdGatewayStatus old_status, MdGatewayStatus new_status)>;
    void set_status_callback(StatusCallback cb) { status_callback_ = std::move(cb); }

protected:
    // Notify registered callback of a status transition (通知注册的回调状态变更)
    void notify_status_change(MdGatewayStatus old_s, MdGatewayStatus new_s) {
        if (status_callback_) status_callback_(old_s, new_s);
    }
    StatusCallback status_callback_;
};

} // namespace hft
