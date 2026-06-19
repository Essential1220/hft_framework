#pragma once
// ============================================
// udp_receiver.h - Standard UDP socket receiver (cross-platform)
// Windows: Winsock2, Linux/macOS: BSD sockets
// ============================================

#include "common/network_receiver.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSock = -1;
#endif

namespace hft {

class UdpReceiver : public INetworkReceiver {
public:
    UdpReceiver();
    ~UdpReceiver() override;

    bool bind(const std::string& address, uint16_t port) override;
    bool join_multicast(const std::string& group) override;
    int recv(void* buf, size_t buf_size, int timeout_ms = -1) override;
    void close() override;
    bool is_open() const override;

private:
    socket_t sock_ = kInvalidSock;
    bool wsa_initialized_ = false;
};

} // namespace hft
