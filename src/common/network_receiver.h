#pragma once
// ============================================
// network_receiver.h - Abstract network receiver interface + factory
// Implementations: UdpReceiver (cross-platform), XDP stub, DPDK stub
// ============================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hft {

class INetworkReceiver {
public:
    virtual ~INetworkReceiver() = default;

    virtual bool bind(const std::string& address, uint16_t port) = 0;
    virtual bool join_multicast(const std::string& group) = 0;
    virtual int recv(void* buf, size_t buf_size, int timeout_ms = -1) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
};

enum class NetworkReceiverType {
    UDP,
    XDP,
    DPDK,
};

std::unique_ptr<INetworkReceiver> create_network_receiver(
    NetworkReceiverType type = NetworkReceiverType::UDP);

} // namespace hft
