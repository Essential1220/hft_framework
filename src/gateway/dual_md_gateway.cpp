// ============================================
// dual_md_gateway.cpp - Dual-active market data gateway implementation
// Primary/backup failover with <1s switchover.
// ============================================

#include "gateway/dual_md_gateway.h"
#include "common/logger.h"

namespace hft {

void DualMdGateway::init(const Config& config, const std::string& section, TradingEngine* engine) {
    engine_ = engine;

    std::string gateway_type = config.get_string("Gateway", "Type", "CTP");
    std::string primary_front = config.get_string(section, "MarketFront", "");
    std::string backup_front = config.get_string(section, "MarketFrontBackup", "");

    if (factory_) {
        primary_ = factory_(gateway_type);
        if (!backup_front.empty()) {
            backup_ = factory_(gateway_type);
        }
    }

    if (primary_) {
        primary_->set_status_callback([this](MdGatewayStatus o, MdGatewayStatus n) {
            on_primary_status(o, n);
        });
        primary_->init(config, section, engine);
    }

    if (backup_ && !backup_front.empty()) {
        Config backup_config = config;
        backup_config.set_string(section, "MarketFront", backup_front);
        backup_->set_status_callback([this](MdGatewayStatus o, MdGatewayStatus n) {
            on_backup_status(o, n);
        });
        backup_->init(backup_config, section, engine);
    }
}

void DualMdGateway::subscribe(const std::vector<std::string>& instruments) {
    {
        std::lock_guard<std::mutex> lock(sub_mtx_);
        subscribed_ = instruments;
    }
    if (using_primary_.load(std::memory_order_relaxed) && primary_) {
        primary_->subscribe(instruments);
    } else if (backup_) {
        backup_->subscribe(instruments);
    }
}

void DualMdGateway::unsubscribe(const std::vector<std::string>& instruments) {
    if (using_primary_.load(std::memory_order_relaxed) && primary_) {
        primary_->unsubscribe(instruments);
    } else if (backup_) {
        backup_->unsubscribe(instruments);
    }
}

void DualMdGateway::subscribe_append(const std::vector<std::string>& instruments) {
    {
        std::lock_guard<std::mutex> lock(sub_mtx_);
        for (const auto& inst : instruments) {
            bool found = false;
            for (const auto& s : subscribed_) {
                if (s == inst) { found = true; break; }
            }
            if (!found) subscribed_.push_back(inst);
        }
    }
    if (using_primary_.load(std::memory_order_relaxed) && primary_) {
        primary_->subscribe_append(instruments);
    } else if (backup_) {
        backup_->subscribe_append(instruments);
    }
}

void DualMdGateway::stop() {
    if (primary_) primary_->stop();
    if (backup_) backup_->stop();
}

bool DualMdGateway::is_logged_in() const {
    if (using_primary_.load(std::memory_order_relaxed) && primary_) {
        return primary_->is_logged_in();
    }
    return backup_ && backup_->is_logged_in();
}

bool DualMdGateway::wait_for_login(int timeout_sec) {
    if (primary_ && primary_->wait_for_login(timeout_sec)) return true;
    if (backup_) return backup_->wait_for_login(timeout_sec);
    return false;
}

MdGatewayStatus DualMdGateway::status() const {
    if (using_primary_.load(std::memory_order_relaxed) && primary_) {
        return primary_->status();
    }
    return backup_ ? backup_->status() : MdGatewayStatus::Disconnected;
}

void DualMdGateway::on_primary_status(MdGatewayStatus old_s, MdGatewayStatus new_s) {
    notify_status_change(old_s, new_s);

    if (new_s == MdGatewayStatus::Disconnected && using_primary_.load(std::memory_order_relaxed)) {
        failover_to_backup();
    } else if (new_s == MdGatewayStatus::LoggedIn && !using_primary_.load(std::memory_order_relaxed)) {
        failback_to_primary();
    }
}

void DualMdGateway::on_backup_status(MdGatewayStatus, MdGatewayStatus) {
}

void DualMdGateway::failover_to_backup() {
    if (!backup_) return;
    LOG_WARN("DualMdGateway: primary disconnected, failing over to backup");
    using_primary_.store(false, std::memory_order_relaxed);

    std::vector<std::string> subs;
    {
        std::lock_guard<std::mutex> lock(sub_mtx_);
        subs = subscribed_;
    }
    if (!subs.empty()) {
        backup_->subscribe(subs);
    }
}

void DualMdGateway::failback_to_primary() {
    LOG_INFO("DualMdGateway: primary reconnected, failing back");
    using_primary_.store(true, std::memory_order_relaxed);

    std::vector<std::string> subs;
    {
        std::lock_guard<std::mutex> lock(sub_mtx_);
        subs = subscribed_;
    }
    if (!subs.empty() && primary_) {
        primary_->subscribe(subs);
    }
    if (backup_) {
        backup_->unsubscribe(subs);
    }
}

} // namespace hft
