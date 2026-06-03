// ============================================
// py_strategy.cpp - Python strategy bridge (Python 策略桥接层)
//
// 核心职责：作为 C++ 底层高频交易框架与 Python 用户策略脚本之间的桥梁。
// 技术栈：使用 pybind11 将 C++ 对象和函数暴露给 Python，同时在 C++ 中嵌入 Python 解释器。
//
// 工作原理：
//   1. 引擎启动时，加载用户指定的 Python 脚本 (load_script)。
//   2. 在脚本运行环境中动态注入一个名为 `hft_engine` 的内建模块，提供 send_order 等 API。
//   3. 将 C++ 引擎产生的回调事件 (Tick, Order, Trade) 转换为 Python 字典 (dict)，
//      并通过调用 Python 脚本中预定义的 `on_tick`, `on_order` 等函数传递给策略。
//
// 线程安全与 GIL (全局解释器锁)：
//   由于 Python 解释器不是完全线程安全的，所有调用 Python 代码的地方 (如 on_tick)
//   都必须使用 `py::gil_scoped_acquire gil;` 获取 GIL，防止多线程崩溃。
// ============================================

#include "strategy/py_strategy.h"

#include "common/logger.h"

#include <atomic>
#include <filesystem>
#include <pybind11/embed.h>
#include <stdexcept>

namespace py = pybind11;

namespace hft {

namespace {

// 辅助函数：将 C++ 的开平标志枚举转换为 Python 侧友好的字符串
const char* offset_to_str(Offset offset) {
    switch (offset) {
        case Offset::Open: return "open";
        case Offset::Close: return "close";
        case Offset::CloseToday: return "close_today";
        case Offset::CloseYesterday: return "close_yesterday";
    }
    return "close";
}

// 全局原子计数器，用于为加载的 Python 策略生成唯一的实例 ID
std::atomic<uint64_t> g_py_strategy_instance_counter{0};

// 线程局部变量，保存当前线程正在执行的 PyStrategy 实例指针。
// 因为 `hft_engine` 模块是全局的，当 Python 脚本调用 `send_order` 时，
// 我们需要知道是哪个策略实例发起的调用，从而正确路由。
thread_local PyStrategy* g_current_py_strategy = nullptr;

} // namespace

// GIL 批量调度状态：consumer_loop 批量获取 GIL 后置为 true，
// 避免每个 PyStrategy 的 on_tick 重复获取/释放 GIL。
thread_local bool g_batch_gil_active = false;

namespace {

struct PyGILGuard : StrategyBase::InterpreterLockGuard {
    py::gil_scoped_acquire gil;
    PyGILGuard() { g_batch_gil_active = true; }
    ~PyGILGuard() override { g_batch_gil_active = false; }
};

} // namespace

} // namespace hft

// ============================================
// 定义嵌入到 Python 环境中的 `hft_engine` 模块
// Python 脚本中可以直接 import hft_engine 并调用这些方法
// ============================================
PYBIND11_EMBEDDED_MODULE(hft_engine, m) {
    // 暴露发单接口
    m.def("send_order", [](py::dict order_dict) -> std::string {
        if (hft::g_current_py_strategy) {
            return hft::g_current_py_strategy->py_send_order(order_dict);
        }
        return "";
    }, "Send an order through the C++ engine, returns order_ref");

    // 暴露撤单接口
    m.def("cancel_order", [](const std::string& order_ref) {
        if (hft::g_current_py_strategy) {
            hft::g_current_py_strategy->py_cancel_order(order_ref);
        }
    }, "Cancel an order by order_ref");

    // 暴露条件单添加接口
    m.def("add_conditional_order", [](py::dict order_dict) -> uint32_t {
        if (hft::g_current_py_strategy) {
            return hft::g_current_py_strategy->py_add_conditional_order(order_dict);
        }
        return 0;
    }, "Add a conditional order");

    // 暴露条件单撤销接口
    m.def("cancel_conditional_order", [](uint32_t id) {
        if (hft::g_current_py_strategy) {
            hft::g_current_py_strategy->py_cancel_conditional_order(id);
        }
    }, "Cancel a conditional order");

    // 暴露分配 OCO 分组 ID 接口
    m.def("allocate_group_id", []() -> uint32_t {
        if (hft::g_current_py_strategy) {
            return hft::g_current_py_strategy->py_allocate_cond_group_id();
        }
        return 0;
    }, "Allocate a new OCO group ID for conditional orders");

    // 暴露查询持仓接口
    m.def("get_position",
          [](const std::string& instrument, const std::string& direction, const std::string& account_id) {
              if (hft::g_current_py_strategy) {
                  return hft::g_current_py_strategy->py_get_position(instrument, direction, account_id);
              }
              return py::dict();
          },
          py::arg("instrument"), py::arg("direction"), py::arg("account_id") = "",
          "Query position snapshot");

    // 暴露查询净持仓接口
    m.def("get_net_position",
          [](const std::string& instrument, const std::string& account_id) -> int {
              if (hft::g_current_py_strategy) {
                  return hft::g_current_py_strategy->py_get_net_position(instrument, account_id);
              }
              return 0;
          },
          py::arg("instrument"), py::arg("account_id") = "",
          "Query net position");

    m.def("get_param",
          [](const std::string& key, const std::string& default_value) -> std::string {
              if (hft::g_current_py_strategy) {
                  return hft::g_current_py_strategy->py_get_param(key, default_value);
              }
              return default_value;
          },
          py::arg("key"), py::arg("default_value") = "",
          "Query strategy string parameter");

    m.def("get_param_int",
          [](const std::string& key, int default_value) -> int {
              if (hft::g_current_py_strategy) {
                  return hft::g_current_py_strategy->py_get_param_int(key, default_value);
              }
              return default_value;
          },
          py::arg("key"), py::arg("default_value") = 0,
          "Query strategy integer parameter");

    m.def("get_param_double",
          [](const std::string& key, double default_value) -> double {
              if (hft::g_current_py_strategy) {
                  return hft::g_current_py_strategy->py_get_param_double(key, default_value);
              }
              return default_value;
          },
          py::arg("key"), py::arg("default_value") = 0.0,
          "Query strategy floating-point parameter");

    m.def("get_strategy_context",
          []() -> py::dict {
              if (hft::g_current_py_strategy) {
                  return hft::g_current_py_strategy->py_get_strategy_context();
              }
              return py::dict();
          },
          "Query loaded strategy context");
}

namespace hft {

PyStrategy::PyStrategy(const std::string& script_path)
    : script_path_(script_path) {
}

PyStrategy::~PyStrategy() {
    if (g_current_py_strategy == this) {
        g_current_py_strategy = nullptr;
    }
    if (!loaded_) return;
    try {
        py::gil_scoped_acquire gil;
        fn_on_init_ = py::object();
        fn_on_tick_ = py::object();
        fn_on_order_ = py::object();
        fn_on_trade_ = py::object();
        fn_on_reconnect_ = py::object();
        fn_on_stop_ = py::object();
        script_module_ = py::module_();
        if (!module_name_.empty()) {
            py::module_ sys = py::module_::import("sys");
            py::dict modules = sys.attr("modules").cast<py::dict>();
            if (modules.contains(module_name_)) {
                modules.attr("__delitem__")(module_name_);
            }
        }
    } catch (...) {}
}

std::unique_ptr<StrategyBase::InterpreterLockGuard> PyStrategy::acquire_interpreter_lock() {
    return std::unique_ptr<InterpreterLockGuard>(new PyGILGuard());
}

// 核心加载逻辑：将 Python 脚本文件加载为独立的 Python 模块
bool PyStrategy::load_script() {
    try {
        const std::filesystem::path path(script_path_);
        const std::string dir = path.parent_path().string();
        const std::string base_name = path.stem().string();
        
        // 为了支持同一个脚本加载多次（不同参数/不同账户），我们需要为模块生成唯一的名称
        const uint64_t instance_id = g_py_strategy_instance_counter.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::string suffix = strategy_id_.empty() ? std::to_string(instance_id) : strategy_id_;
        const std::string module_name = base_name + "__" + suffix;
        module_name_ = module_name;

        // 获取 sys.path 并将脚本所在目录加入其中，确保能够 import 同目录下的其他文件
        py::module_ sys = py::module_::import("sys");
        py::list py_path = sys.attr("path").cast<py::list>();
        bool path_exists = false;
        for (const py::handle& item : py_path) {
            if (py::cast<std::string>(item) == dir) {
                path_exists = true;
                break;
            }
        }
        if (!path_exists) {
            py_path.append(dir);
        }

        // 使用 importlib 动态加载脚本文件
        py::module_ importlib_util = py::module_::import("importlib.util");
        py::object spec = importlib_util.attr("spec_from_file_location")(module_name, path.string());
        if (spec.is_none()) {
            throw std::runtime_error("failed to create Python module spec");
        }

        // 创建并执行模块
        py::object module = importlib_util.attr("module_from_spec")(spec);
        sys.attr("modules")[py::str(module_name)] = module;
        spec.attr("loader").attr("exec_module")(module);
        script_module_ = module.cast<py::module_>();

        // ---- 绑定 Python 脚本中定义的生命周期函数 ----
        // 使用 PyObject_GetAttrString 检查属性是否存在，避免因为脚本没实现某个函数而抛出异常
        {
            PyObject* attr = PyObject_GetAttrString(script_module_.ptr(), "on_init");
            if (attr) { fn_on_init_ = py::reinterpret_steal<py::object>(attr); }
            else { PyErr_Clear(); fn_on_init_ = py::none(); }
        }
        {
            PyObject* attr = PyObject_GetAttrString(script_module_.ptr(), "on_tick");
            if (attr) { fn_on_tick_ = py::reinterpret_steal<py::object>(attr); }
            else { PyErr_Clear(); fn_on_tick_ = py::none(); }
        }
        {
            PyObject* attr = PyObject_GetAttrString(script_module_.ptr(), "on_order");
            if (attr) { fn_on_order_ = py::reinterpret_steal<py::object>(attr); }
            else { PyErr_Clear(); fn_on_order_ = py::none(); }
        }
        {
            PyObject* attr = PyObject_GetAttrString(script_module_.ptr(), "on_trade");
            if (attr) { fn_on_trade_ = py::reinterpret_steal<py::object>(attr); }
            else { PyErr_Clear(); fn_on_trade_ = py::none(); }
        }
        {
            PyObject* attr = PyObject_GetAttrString(script_module_.ptr(), "on_reconnect");
            if (attr) { fn_on_reconnect_ = py::reinterpret_steal<py::object>(attr); }
            else { PyErr_Clear(); fn_on_reconnect_ = py::none(); }
        }
        {
            PyObject* attr = PyObject_GetAttrString(script_module_.ptr(), "on_stop");
            if (attr) { fn_on_stop_ = py::reinterpret_steal<py::object>(attr); }
            else { PyErr_Clear(); fn_on_stop_ = py::none(); }
        }

        loaded_ = true;
        LOG_INFO("PyStrategy script loaded: " + script_path_ + " module=" + module_name);
        return true;
    } catch (const py::error_already_set& e) {
        LOG_ERROR("PyStrategy script load failed: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("PyStrategy script load failed: " + std::string(e.what()));
        return false;
    }
}

// ---- 回调事件的 C++ -> Python 转发层 ----

void PyStrategy::on_init() {
    // 绑定当前线程上下文
    g_current_py_strategy = this;

    // 必须获取 GIL
    py::gil_scoped_acquire gil;
    // 延迟加载：在收到 on_init 时才真正去解析 Python 脚本
    if (!loaded_ && !load_script()) {
        throw std::runtime_error("PyStrategy script load failed: " + script_path_);
    }

    if (!fn_on_init_.is_none()) {
        try {
            fn_on_init_();
        } catch (const py::error_already_set& e) {
            LOG_ERROR("PyStrategy::on_init exception: " + std::string(e.what()));
        }
    }
}

void PyStrategy::on_tick(const TickData& tick) {
    if (!loaded_ || fn_on_tick_.is_none()) {
        return;
    }

    g_current_py_strategy = this;

    if (g_batch_gil_active) {
        try {
            fn_on_tick_(tick_to_dict(tick));
        } catch (const py::error_already_set& e) {
            LOG_ERROR("PyStrategy::on_tick exception: " + std::string(e.what()));
        }
        return;
    }

    py::gil_scoped_acquire gil;
    try {
        fn_on_tick_(tick_to_dict(tick));
    } catch (const py::error_already_set& e) {
        LOG_ERROR("PyStrategy::on_tick exception: " + std::string(e.what()));
    }
}

void PyStrategy::on_order(const OrderInfo& order) {
    if (!loaded_ || fn_on_order_.is_none()) {
        return;
    }

    g_current_py_strategy = this;

    if (g_batch_gil_active) {
        try {
            fn_on_order_(order_to_dict(order));
        } catch (const py::error_already_set& e) {
            LOG_ERROR("PyStrategy::on_order exception: " + std::string(e.what()));
        }
        return;
    }

    py::gil_scoped_acquire gil;
    try {
        fn_on_order_(order_to_dict(order));
    } catch (const py::error_already_set& e) {
        LOG_ERROR("PyStrategy::on_order exception: " + std::string(e.what()));
    }
}

void PyStrategy::on_trade(const TradeInfo& trade) {
    if (!loaded_ || fn_on_trade_.is_none()) {
        return;
    }

    g_current_py_strategy = this;

    if (g_batch_gil_active) {
        try {
            fn_on_trade_(trade_to_dict(trade));
        } catch (const py::error_already_set& e) {
            LOG_ERROR("PyStrategy::on_trade exception: " + std::string(e.what()));
        }
        return;
    }

    py::gil_scoped_acquire gil;
    try {
        fn_on_trade_(trade_to_dict(trade));
    } catch (const py::error_already_set& e) {
        LOG_ERROR("PyStrategy::on_trade exception: " + std::string(e.what()));
    }
}

void PyStrategy::on_reconnect() {
    if (!loaded_ || fn_on_reconnect_.is_none()) {
        return;
    }

    g_current_py_strategy = this;
    py::gil_scoped_acquire gil;
    try {
        fn_on_reconnect_();
    } catch (const py::error_already_set& e) {
        LOG_ERROR("PyStrategy::on_reconnect exception: " + std::string(e.what()));
    }
}

void PyStrategy::on_stop() {
    if (!loaded_ || fn_on_stop_.is_none()) {
        return;
    }

    g_current_py_strategy = this;
    py::gil_scoped_acquire gil;
    try {
        fn_on_stop_();
    } catch (const py::error_already_set& e) {
        LOG_ERROR("PyStrategy::on_stop exception: " + std::string(e.what()));
    }
}

// ---- API 的 Python -> C++ 转发层 ----

// 处理 Python 侧调用的 hft_engine.send_order()
std::string PyStrategy::py_send_order(const py::dict& d) {
    OrderRequest req{};

    // 从 Python dict 中提取并转换各个字段
    if (d.contains("instrument_id")) {
        safe_copy(req.instrument_id, d["instrument_id"].cast<std::string>().c_str(), sizeof(req.instrument_id));
    }
    if (d.contains("exchange_id")) {
        safe_copy(req.exchange_id, d["exchange_id"].cast<std::string>().c_str(), sizeof(req.exchange_id));
    } else {
        // 如果未指定交易所代码，则根据合约名自动推断
        safe_copy(req.exchange_id, get_exchange_id(req.instrument_id), sizeof(req.exchange_id));
    }

    if (d.contains("direction")) {
        const std::string dir = d["direction"].cast<std::string>();
        req.direction = (dir == "sell" || dir == "Sell") ? Direction::Sell : Direction::Buy;
    }
    if (d.contains("offset")) {
        const std::string off = d["offset"].cast<std::string>();
        if (off == "close" || off == "Close") req.offset = Offset::Close;
        else if (off == "close_today" || off == "CloseToday") req.offset = Offset::CloseToday;
        else if (off == "close_yesterday" || off == "CloseYesterday") req.offset = Offset::CloseYesterday;
        else req.offset = Offset::Open;
    }
    if (d.contains("price")) req.price = d["price"].cast<double>();
    if (d.contains("volume")) req.volume = d["volume"].cast<int>();

    // 如果 Python 没有显式传递账户和策略 ID，底层发单引擎会自动补全当前绑定的信息
    if (d.contains("account_id")) {
        safe_copy(req.account_id, d["account_id"].cast<std::string>().c_str(), sizeof(req.account_id));
    }
    if (d.contains("strategy_id")) {
        safe_copy(req.strategy_id, d["strategy_id"].cast<std::string>().c_str(), sizeof(req.strategy_id));
    }

    // 调用 C++ 基类的发送接口，返回 order_ref
    return send_order(req);
}

void PyStrategy::py_cancel_order(const std::string& order_ref) {
    cancel_order(order_ref);
}

uint32_t PyStrategy::py_add_conditional_order(const py::dict& d) {
    ConditionalOrder order{};

    // 解析条件单基础信息
    if (d.contains("instrument_id")) {
        safe_copy(order.instrument_id, d["instrument_id"].cast<std::string>().c_str(), sizeof(order.instrument_id));
    }
    if (d.contains("account_id")) {
        safe_copy(order.account_id, d["account_id"].cast<std::string>().c_str(), sizeof(order.account_id));
    }
    if (d.contains("strategy_id")) {
        safe_copy(order.strategy_id, d["strategy_id"].cast<std::string>().c_str(), sizeof(order.strategy_id));
    }

    // 解析条件单类型 (止损、止盈、跟踪止损)
    if (d.contains("type")) {
        const std::string t = d["type"].cast<std::string>();
        if (t == "stop_loss" || t == "StopLoss") order.type = ConditionType::StopLoss;
        else if (t == "take_profit" || t == "TakeProfit") order.type = ConditionType::TakeProfit;
        else if (t == "trailing_stop" || t == "TrailingStop") order.type = ConditionType::TrailingStop;
    }

    if (d.contains("direction")) {
        const std::string dir = d["direction"].cast<std::string>();
        order.direction = (dir == "sell" || dir == "Sell") ? Direction::Sell : Direction::Buy;
    }

    // 解析触发条件和数值
    if (d.contains("trigger_price")) order.trigger_price = d["trigger_price"].cast<double>();
    if (d.contains("order_price")) order.order_price = d["order_price"].cast<double>();
    if (d.contains("trail_offset")) order.trail_offset = d["trail_offset"].cast<double>();
    if (d.contains("volume")) order.volume = d["volume"].cast<int>();
    if (d.contains("group_id")) order.group_id = d["group_id"].cast<uint32_t>();

    return add_conditional_order(order);
}

void PyStrategy::py_cancel_conditional_order(uint32_t id) {
    cancel_conditional_order(id);
}

uint32_t PyStrategy::py_allocate_cond_group_id() {
    return allocate_cond_group_id();
}

py::dict PyStrategy::py_get_position(const std::string& instrument, const std::string& direction,
                                     const std::string& account_id) {
    const Direction dir = (direction == "sell" || direction == "Sell") ? Direction::Sell : Direction::Buy;
    // 如果 Python 未传递 account_id，则查询当前策略绑定的默认持仓
    const PositionInfo pos = account_id.empty()
        ? get_position(instrument.c_str(), dir)
        : get_position(instrument.c_str(), dir, account_id.c_str());

    // 构造并返回包含持仓明细的字典
    py::dict d;
    d["instrument_id"] = pos.instrument_id;
    d["direction"] = (pos.direction == Direction::Buy) ? "buy" : "sell";
    d["total"] = pos.total;
    d["today"] = pos.today;
    d["yesterday"] = pos.yesterday;
    d["avg_price"] = pos.avg_price;
    d["position_profit"] = pos.position_profit;
    d["use_margin"] = pos.use_margin;
    d["account_id"] = pos.account_id;
    return d;
}

int PyStrategy::py_get_net_position(const std::string& instrument, const std::string& account_id) {
    return account_id.empty()
        ? get_net_position(instrument.c_str())
        : get_net_position(instrument.c_str(), account_id.c_str());
}

std::string PyStrategy::py_get_param(const std::string& key, const std::string& default_value) {
    return get_parameter(key, default_value);
}

int PyStrategy::py_get_param_int(const std::string& key, int default_value) {
    return get_parameter_int(key, default_value);
}

double PyStrategy::py_get_param_double(const std::string& key, double default_value) {
    return get_parameter_double(key, default_value);
}

py::dict PyStrategy::py_get_strategy_context() {
    py::dict d;
    d["strategy_id"] = strategy_id();
    d["strategy_type"] = strategy_type();
    d["script_path"] = script_path();
    d["account_id"] = default_account_id();

    py::list instruments;
    for (const auto& instrument : watched_instruments()) {
        instruments.append(instrument);
    }
    d["instruments"] = instruments;

    py::dict params;
    for (const auto& [key, value] : parameters()) {
        params[py::str(key)] = py::str(value);
    }
    d["params"] = params;
    return d;
}

// ---- 数据结构转换辅助函数 (C++ -> Python Dict) ----
// 避免在 Python 端使用难以调试的 C++ 包装对象，全部退化为基础 dict

py::dict PyStrategy::tick_to_dict(const TickData& t) {
    py::dict d;
    d["instrument_id"] = t.instrument_id;
    d["exchange_id"] = t.exchange_id;
    d["last_price"] = t.last_price;
    d["pre_close_price"] = t.pre_close_price;
    d["open_price"] = t.open_price;
    d["highest_price"] = t.highest_price;
    d["lowest_price"] = t.lowest_price;
    d["volume"] = t.volume;
    d["turnover"] = t.turnover;
    d["open_interest"] = t.open_interest;
    d["bid_price1"] = t.bid[0].price;
    d["bid_volume1"] = t.bid[0].volume;
    d["ask_price1"] = t.ask[0].price;
    d["ask_volume1"] = t.ask[0].volume;
    d["upper_limit"] = t.upper_limit;
    d["lower_limit"] = t.lower_limit;
    d["update_time"] = t.update_time;
    d["update_millisec"] = t.update_millisec;
    d["trading_day"] = t.trading_day;
    d["action_day"] = t.action_day;
    d["local_recv_ns"] = t.local_recv_ns;
    return d;
}

py::dict PyStrategy::order_to_dict(const OrderInfo& o) {
    py::dict d;
    d["instrument_id"] = o.instrument_id;
    d["exchange_id"] = o.exchange_id;
    d["order_ref"] = o.order_ref;
    d["order_sys_id"] = o.order_sys_id;
    d["direction"] = (o.direction == Direction::Buy) ? "buy" : "sell";
    d["offset"] = offset_to_str(o.offset);
    d["price"] = o.price;
    d["total_volume"] = o.total_volume;
    d["traded_volume"] = o.traded_volume;
    d["status"] = static_cast<int>(to_legacy_status(o.status));
    d["status_msg"] = o.status_msg;
    d["insert_time"] = o.insert_time;
    d["account_id"] = o.account_id;
    d["strategy_id"] = o.strategy_id;
    return d;
}

py::dict PyStrategy::trade_to_dict(const TradeInfo& t) {
    py::dict d;
    d["instrument_id"] = t.instrument_id;
    d["exchange_id"] = t.exchange_id;
    d["trade_id"] = t.trade_id;
    d["order_ref"] = t.order_ref;
    d["direction"] = (t.direction == Direction::Buy) ? "buy" : "sell";
    d["offset"] = offset_to_str(t.offset);
    d["price"] = t.price;
    d["volume"] = t.volume;
    d["trade_time"] = t.trade_time;
    d["account_id"] = t.account_id;
    d["strategy_id"] = t.strategy_id;
    return d;
}

} // namespace hft
