// ============================================
// udp_md_gateway.cpp - UDP multicast market data gateway implementation
// ============================================

#include "gateway/udp_md_gateway.h"
#include "common/logger.h"
#include "engine/trading_engine.h"

#include <cstring>

namespace hft {

void UdpMdGateway::init(const Config& config, const std::string& section, TradingEngine* engine) {
    engine_ = engine;
    multicast_group_ = config.get_string("UDP", "MulticastGroup", "239.1.1.1");
    port_ = static_cast<uint16_t>(config.get_int("UDP", "Port", 5555));

    receiver_ = create_network_receiver(NetworkReceiverType::UDP);
    if (!receiver_) {
        LOG_ERROR("UdpMdGateway: failed to create UDP receiver");
        return;
    }

    if (!receiver_->bind("0.0.0.0", port_)) {
        LOG_ERROR("UdpMdGateway: failed to bind to port " + std::to_string(port_));
        return;
    }

    if (!receiver_->join_multicast(multicast_group_)) {
        LOG_WARN("UdpMdGateway: failed to join multicast group " + multicast_group_);
    }

    running_.store(true, std::memory_order_relaxed);
    status_.store(MdGatewayStatus::LoggedIn, std::memory_order_relaxed);
    recv_thread_ = std::thread([this]() { recv_loop(); });

    LOG_INFO("UdpMdGateway: listening on " + multicast_group_ + ":" + std::to_string(port_));
}

void UdpMdGateway::subscribe(const std::vector<std::string>&) {}
void UdpMdGateway::unsubscribe(const std::vector<std::string>&) {}

void UdpMdGateway::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (receiver_) receiver_->close();
    if (recv_thread_.joinable()) recv_thread_.join();
    status_.store(MdGatewayStatus::Disconnected, std::memory_order_relaxed);
}

bool UdpMdGateway::is_logged_in() const {
    return status_.load(std::memory_order_relaxed) == MdGatewayStatus::LoggedIn;
}

bool UdpMdGateway::wait_for_login(int) {
    return is_logged_in();
}

MdGatewayStatus UdpMdGateway::status() const {
    return status_.load(std::memory_order_relaxed);
}

void UdpMdGateway::recv_loop() {
    constexpr size_t kBufSize = sizeof(UdpTickHeader) + sizeof(TickData);
    char buf[kBufSize];

    while (running_.load(std::memory_order_relaxed)) {
        int n = receiver_->recv(buf, kBufSize, 100);
        if (n < static_cast<int>(sizeof(UdpTickHeader))) continue;

        UdpTickHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(hdr));

        if (std::memcmp(hdr.magic, kUdpTickMagic, 4) != 0) continue;
        if (hdr.payload_size != sizeof(TickData)) continue;
        if (n < static_cast<int>(sizeof(UdpTickHeader) + sizeof(TickData))) continue;

        if (hdr.sequence <= last_seq_ && last_seq_ > 0) {
            // Possible duplicate or reordering
        }
        last_seq_ = hdr.sequence;

        TickData tick{};
        std::memcpy(&tick, buf + sizeof(UdpTickHeader), sizeof(TickData));

        if (engine_) {
            engine_->on_tick(tick);
        }
    }
}

} // namespace hft
