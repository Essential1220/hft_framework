// ============================================
// instrument_registry.cpp - Instrument registry implementation (合约注册表实现)
// ============================================

#include "engine/instrument_registry.h"

#include "common/string_utils.h"

#include <algorithm>

namespace hft {

namespace {

// Extract product prefix from instrument code (从合约代码中提取品种前缀)
std::string product_from_instrument_local(const std::string& instrument) {
    size_t index = 0;
    while (index < instrument.size() && std::isalpha(static_cast<unsigned char>(instrument[index]))) {
        ++index;
    }
    std::string p = instrument.substr(0, index);
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return p;
}

} // namespace

// Get instrument specs — if instrument is empty, return all sorted by ID (获取合约规格, 若 instrument 为空则返回全部按 ID 排序)
std::vector<InstrumentSpec> InstrumentRegistry::get_specs(const std::string& instrument) const {
    const std::string filter = trim_copy(instrument);
    std::vector<InstrumentSpec> specs;
    if (!filter.empty()) {
        const auto it = instrument_specs_.find(filter);
        specs.push_back(it != instrument_specs_.end() ? it->second : InstrumentSpec{});
        return specs;
    }
    for (const auto& [_, spec] : instrument_specs_) {
        specs.push_back(spec);
    }
    std::sort(specs.begin(), specs.end(), [](const InstrumentSpec& a, const InstrumentSpec& b) {
        return a.instrument_id < b.instrument_id;
    });
    return specs;
}

// Check if instrument exists in registry (检查合约是否在注册表中)
bool InstrumentRegistry::has_instrument(const std::string& instrument) const {
    const std::string inst = trim_copy(instrument);
    if (instrument_specs_.find(inst) != instrument_specs_.end()) return true;
    for (const auto& i : instruments_) {
        if (i == inst) return true;
    }
    return false;
}

void InstrumentRegistry::update_spec(const InstrumentSpec& spec) {
    if (trim_copy(spec.instrument_id).empty()) return;
    instrument_specs_[spec.instrument_id] = spec;
}

std::vector<std::string> InstrumentRegistry::get_market_universe() const {
    return market_universe_;
}

bool InstrumentRegistry::is_hot(const char* instrument) const {
    if (!instrument || instrument[0] == '\0') return false;
    auto snap = std::atomic_load_explicit(&hot_instruments_, std::memory_order_acquire);
    if (!snap) return false;
    return snap->find(InstrumentKey(instrument)) != snap->end();
}

bool InstrumentRegistry::register_hot(const std::string& instrument) {
    if (instrument.empty()) return false;
    std::lock_guard<std::mutex> lock(hot_write_mtx_);
    auto current = std::atomic_load_explicit(&hot_instruments_, std::memory_order_acquire);
    auto next = current ? std::make_shared<HotSet>(*current) : std::make_shared<HotSet>();
    const bool inserted = next->insert(InstrumentKey(instrument.c_str())).second;
    if (!inserted) return false;
    std::atomic_store_explicit(
        &hot_instruments_,
        std::shared_ptr<const HotSet>(std::move(next)),
        std::memory_order_release);
    return true;
}

void InstrumentRegistry::rebuild_hot(const std::vector<std::string>& strategy_instruments,
                                     const std::vector<std::string>& config_instruments,
                                     const std::vector<std::string>& cond_order_instruments) {
    auto next = std::make_shared<HotSet>();
    for (const auto& inst : strategy_instruments) {
        if (!inst.empty()) next->insert(InstrumentKey(inst.c_str()));
    }
    for (const auto& inst : config_instruments) {
        if (!inst.empty()) next->insert(InstrumentKey(inst.c_str()));
    }
    for (const auto& inst : cond_order_instruments) {
        if (!inst.empty()) next->insert(InstrumentKey(inst.c_str()));
    }
    std::lock_guard<std::mutex> lock(hot_write_mtx_);
    std::atomic_store_explicit(
        &hot_instruments_,
        std::shared_ptr<const HotSet>(std::move(next)),
        std::memory_order_release);
}

void InstrumentRegistry::set_strategy_instruments(std::vector<std::string> instruments) {
    instruments_ = std::move(instruments);
}

void InstrumentRegistry::set_market_universe(std::vector<std::string> universe) {
    market_universe_ = std::move(universe);
}

void InstrumentRegistry::set_specs(std::map<std::string, InstrumentSpec> specs) {
    instrument_specs_ = std::move(specs);
}

void InstrumentRegistry::clear_specs() {
    instrument_specs_.clear();
}

// Infer instrument spec from instrument code — determines exchange, tick size, multiplier, etc.
// (从合约代码推断合约规格 — 确定交易所、最小变动价位、合约乘数等)
InstrumentSpec infer_instrument_spec(const std::string& instrument) {
    InstrumentSpec spec;
    spec.instrument_id = instrument;
    spec.product_id = product_from_instrument_local(instrument);
    const std::string& p = spec.product_id;
    if (p == "if" || p == "ih" || p == "ic" || p == "im" ||
        p == "io" || p == "mo" || p == "ho") {
        spec.exchange_id = "CFFEX";
        spec.price_tick = 0.2;
        spec.volume_multiple = (p == "io" || p == "mo" || p == "ho") ? 100 : ((p == "ih") ? 300 : (p == "if" ? 300 : 200));
        spec.long_margin_ratio = 0.12;
        spec.short_margin_ratio = 0.12;
        spec.open_commission_rate = 0.000023;
        spec.close_commission_rate = 0.000023;
        spec.close_today_commission_rate = 0.00023;
    } else if (p == "sc" || p == "lu" || p == "bc" || p == "nr") {
        spec.exchange_id = "INE";
        spec.price_tick = (p == "sc") ? 0.1 : 1.0;
        spec.volume_multiple = (p == "sc") ? 1000 : 10;
        spec.long_margin_ratio = 0.10;
        spec.short_margin_ratio = 0.10;
    } else if (p == "au" || p == "ag" || p == "cu" || p == "al" || p == "zn" || p == "pb" || p == "ni" || p == "sn" || p == "rb" || p == "hc" || p == "ss" || p == "bu" || p == "ru" || p == "sp") {
        spec.exchange_id = "SHFE";
        spec.price_tick = (p == "au") ? 0.02 : ((p == "ag") ? 1.0 : 5.0);
        spec.volume_multiple = (p == "au") ? 1000 : ((p == "ag") ? 15 : 10);
        spec.long_margin_ratio = 0.10;
        spec.short_margin_ratio = 0.10;
    } else if (p == "si" || p == "lc") {
        spec.exchange_id = "GFEX";
        spec.price_tick = 1.0;
        spec.volume_multiple = 5;
        spec.long_margin_ratio = 0.10;
        spec.short_margin_ratio = 0.10;
    } else if (p == "m" || p == "y" || p == "p" || p == "c" || p == "cs" || p == "a" || p == "b" || p == "i" || p == "j" || p == "jm" || p == "l" || p == "v" || p == "pp" || p == "eg" || p == "eb" || p == "pg" || p == "lh" || p == "jd") {
        spec.exchange_id = "DCE";
        spec.price_tick = (p == "i" || p == "j" || p == "jm") ? 0.5 : 1.0;
        spec.volume_multiple = (p == "jd") ? 5 : 10;
        spec.long_margin_ratio = 0.10;
        spec.short_margin_ratio = 0.10;
    } else {
        spec.exchange_id = "CZCE";
        spec.price_tick = 1.0;
        spec.volume_multiple = 10;
        spec.long_margin_ratio = 0.10;
        spec.short_margin_ratio = 0.10;
    }
    return spec;
}

// Apply config overrides to instrument spec (将配置文件中的覆盖项应用到合约规格)
InstrumentSpec apply_instrument_spec_overrides(const Config& config, InstrumentSpec spec) {
    if (spec.instrument_id.empty()) return spec;
    const std::string section = "Instrument." + spec.instrument_id;
    if (!config.has_section(section)) return spec;
    const std::string exchange = trim_copy(config.get_string(section, "ExchangeID", ""));
    const std::string product = trim_copy(config.get_string(section, "ProductID", ""));
    if (!exchange.empty()) spec.exchange_id = exchange;
    if (!product.empty()) spec.product_id = product;
    const std::string expire_date = trim_copy(config.get_string(section, "ExpireDate", ""));
    const std::string start_deliv_date = trim_copy(config.get_string(section, "StartDelivDate", ""));
    const std::string end_deliv_date = trim_copy(config.get_string(section, "EndDelivDate", ""));
    const std::string inst_life_phase = trim_copy(config.get_string(section, "InstLifePhase", ""));
    if (!expire_date.empty()) spec.expire_date = expire_date;
    if (!start_deliv_date.empty()) spec.start_deliv_date = start_deliv_date;
    if (!end_deliv_date.empty()) spec.end_deliv_date = end_deliv_date;
    if (!inst_life_phase.empty()) spec.inst_life_phase = inst_life_phase[0];
    spec.is_trading = config.get_int(section, "IsTrading", spec.is_trading ? 1 : 0) != 0;
    spec.price_tick = config.get_double(section, "PriceTick", spec.price_tick);
    spec.volume_multiple = config.get_int(section, "VolumeMultiple", spec.volume_multiple);
    spec.long_margin_ratio = config.get_double(section, "LongMarginRatio", spec.long_margin_ratio);
    spec.short_margin_ratio = config.get_double(section, "ShortMarginRatio", spec.short_margin_ratio);
    spec.open_commission_rate = config.get_double(section, "OpenCommissionRate", spec.open_commission_rate);
    spec.close_commission_rate = config.get_double(section, "CloseCommissionRate", spec.close_commission_rate);
    spec.close_today_commission_rate = config.get_double(section, "CloseTodayCommissionRate", spec.close_today_commission_rate);
    return spec;
}

} // namespace hft
