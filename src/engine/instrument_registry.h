#pragma once
// ============================================
// instrument_registry.h - Instrument registry and hot-set management (合约注册与热路径集合管理)
// Maintains the market universe, instrument specs, and a hot-set for fast
// lock-free lookup on the critical path.
// (维护行情合约范围、合约规格以及热路径无锁快速查找的热点集合)
// ============================================

#include "common/config.h"
#include "common/types.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace hft {

class InstrumentRegistry {
public:
    std::vector<InstrumentSpec> get_specs(const std::string& instrument = "") const;
    bool has_instrument(const std::string& instrument) const;
    void update_spec(const InstrumentSpec& spec);
    std::vector<std::string> get_market_universe() const;

    bool is_hot(const char* instrument) const;
    bool register_hot(const std::string& instrument);
    void rebuild_hot(const std::vector<std::string>& strategy_instruments,
                     const std::vector<std::string>& config_instruments,
                     const std::vector<std::string>& cond_order_instruments);

    void set_strategy_instruments(std::vector<std::string> instruments);
    void set_market_universe(std::vector<std::string> universe);
    void set_specs(std::map<std::string, InstrumentSpec> specs);
    void clear_specs();
    std::map<std::string, InstrumentSpec>& specs_mut() { return instrument_specs_; }
    const std::map<std::string, InstrumentSpec>& specs() const { return instrument_specs_; }
    std::vector<std::string>& instruments_ref() { return instruments_; }
    const std::vector<std::string>& instruments_ref() const { return instruments_; }
    std::vector<std::string>& market_universe_ref() { return market_universe_; }
    const std::vector<std::string>& market_universe_ref() const { return market_universe_; }

private:
    using HotSet = std::unordered_set<InstrumentKey, InstrumentKeyHash>;

    std::vector<std::string> instruments_;
    std::vector<std::string> market_universe_;
    std::map<std::string, InstrumentSpec> instrument_specs_;
    // Hot-path is_hot() uses atomic_load(shared_ptr<const HotSet>), zero lock.
    // Writer (register_hot/rebuild_hot) does copy-on-write under hot_write_mtx_,
    // then atomic_store. Read-heavy, write-extremely-rare.
    // (热路径 is_hot() 用 atomic_load(shared_ptr<const HotSet>),零锁。
    //  写者 register_hot/rebuild_hot 在 hot_write_mtx_ 下做 copy-on-write 后
    //  atomic_store。读多写极少。)
    std::shared_ptr<const HotSet> hot_instruments_;
    mutable std::mutex hot_write_mtx_;
};

InstrumentSpec infer_instrument_spec(const std::string& instrument);
InstrumentSpec apply_instrument_spec_overrides(const Config& config, InstrumentSpec spec);

} // namespace hft
