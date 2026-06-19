#pragma once
// ============================================
// py_strategy.h - Python strategy bridge class (Python 策略桥接类)
// Calls Python strategy functions from C++ callbacks (在 C++ 回调中调用 Python 策略函数)
// ============================================

#include "strategy/strategy_base.h"
#include <pybind11/embed.h>
#include <string>

namespace py = pybind11;

namespace hft {

class PyStrategy : public StrategyBase {
public:
    // 构造函数，传入 Python 脚本路径
    explicit PyStrategy(const std::string& script_path);
    ~PyStrategy() override;

    // ---- 实现 StrategyBase 接口，转发调用给 Python 层 ----
    void on_init() override;
    void on_tick(const TickData& tick) override;
    void on_order(const OrderInfo& order) override;
    void on_trade(const TradeInfo& trade) override;
    void on_reconnect() override;
    void on_stop() override;
    void on_bar(const std::string& instrument, const std::string& period, const KlineBar& bar) override;
    void on_timer(int timer_id) override;

    bool is_interpreted() const override { return true; }
    std::unique_ptr<InterpreterLockGuard> acquire_interpreter_lock() override;

    // ---- 供 Python 调用的 API 接口 ----
    std::string py_send_order(const py::dict& order_dict);
    void py_cancel_order(const std::string& order_ref);
    uint32_t py_add_conditional_order(const py::dict& order_dict);
    void py_cancel_conditional_order(uint32_t id);
    uint32_t py_allocate_cond_group_id();
    py::dict py_get_position(const std::string& instrument, const std::string& direction,
                             const std::string& account_id = "");
    int py_get_net_position(const std::string& instrument, const std::string& account_id = "");
    std::string py_get_param(const std::string& key, const std::string& default_value = "");
    int py_get_param_int(const std::string& key, int default_value = 0);
    double py_get_param_double(const std::string& key, double default_value = 0.0);
    py::dict py_get_strategy_context();
    void py_log_info(const std::string& msg);
    void py_log_warn(const std::string& msg);
    void py_log_error(const std::string& msg);
    void py_save_state(const py::dict& state);
    py::dict py_load_state();
    py::dict py_get_account_info(const std::string& account_id = "");
    int py_register_timer(int interval_ms);
    void py_unregister_timer(int timer_id);
    py::dict py_get_order_book(const std::string& instrument);
    py::list py_query_klines(const std::string& instrument, const std::string& period, size_t count = 200);

private:
    // 加载 Python 脚本并缓存函数对象
    bool load_script();

    // C++ 结构体转 Python dict
    static py::dict tick_to_dict(const TickData& tick);
    static py::dict order_to_dict(const OrderInfo& order);
    static py::dict trade_to_dict(const TradeInfo& trade);
    static py::dict bar_to_dict(const KlineBar& bar);

    std::string script_path_; // Python 脚本绝对或相对路径
    std::string module_name_; // sys.modules 中的模块名（用于卸载时清理）
    py::module_ script_module_; // 导入的 Python 模块

    // 缓存的 Python 函数对象，避免每次回调时查找
    py::object fn_on_init_;
    py::object fn_on_tick_;
    py::object fn_on_order_;
    py::object fn_on_trade_;
    py::object fn_on_reconnect_;
    py::object fn_on_stop_;
    py::object fn_on_bar_;
    py::object fn_on_timer_;

    bool loaded_ = false; // 脚本是否成功加载
};

} // namespace hft
