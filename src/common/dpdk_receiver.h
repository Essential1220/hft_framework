#pragma once
// ============================================
// dpdk_receiver.h - DPDK kernel bypass receiver stub
// Requires DPDK libraries and HFT_HAS_DPDK=ON. TODO: full implementation.
// ============================================

#ifdef HFT_HAS_DPDK

#include "common/network_receiver.h"

namespace hft {

class DpdkReceiver : public INetworkReceiver {
public:
    bool bind(const std::string&, uint16_t) override { return false; }
    bool join_multicast(const std::string&) override { return false; }
    int recv(void*, size_t, int) override { return -1; }
    void close() override {}
    bool is_open() const override { return false; }
    // TODO: EAL init, mempool, rx queue setup, rte_eth_rx_burst loop
};

} // namespace hft

#endif // HFT_HAS_DPDK
