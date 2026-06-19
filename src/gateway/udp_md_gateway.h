#pragma once
// ============================================
// udp_md_gateway.h - UDP multicast market data gateway
// Machine B: receives TickData from multicast group published by UdpMdPublisher.
// ============================================

#include "gateway/i_md_gateway.h"
#include "gateway/udp_md_publisher.h"
#include "common/network_receiver.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hft {

class UdpMdGateway : public IMdGateway {
public:
    ~UdpMdGateway() override { stop(); }

    void init(const Config& config, const std::string& section, TradingEngine* engine) override;
    void subscribe(const std::vector<std::string>& instruments) override;
    void unsubscribe(const std::vector<std::string>& instruments) override;
    void stop() override;
    bool is_logged_in() const override;
    bool wait_for_login(int timeout_sec = 10) override;
    MdGatewayStatus status() const override;

private:
    void recv_loop();

    TradingEngine* engine_ = nullptr;
    std::unique_ptr<INetworkReceiver> receiver_;
    std::thread recv_thread_;
    std::atomic<bool> running_{false};
    std::atomic<MdGatewayStatus> status_{MdGatewayStatus::Disconnected};
    std::string multicast_group_;
    uint16_t port_ = 5555;
    uint32_t last_seq_ = 0;
};

} // namespace hft
