#pragma once
// ============================================
// afxdp_receiver.h - AF_XDP kernel bypass receiver stub (Linux only)
// Requires AF_XDP userspace library. TODO: full implementation.
// ============================================

#ifdef __linux__

#include "common/network_receiver.h"

namespace hft {

class AfXdpReceiver : public INetworkReceiver {
public:
    bool bind(const std::string&, uint16_t) override { return false; }
    bool join_multicast(const std::string&) override { return false; }
    int recv(void*, size_t, int) override { return -1; }
    void close() override {}
    bool is_open() const override { return false; }
    // TODO: XSK socket setup, UMEM configuration, BPF program loading
};

} // namespace hft

#endif // __linux__
