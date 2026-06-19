// ============================================
// udp_receiver.cpp - Standard UDP socket receiver implementation
// ============================================

#include "common/udp_receiver.h"

#include <cstring>

#ifdef _WIN32
#else
#include <poll.h>
#endif

namespace hft {

UdpReceiver::UdpReceiver() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        wsa_initialized_ = true;
    }
#endif
}

UdpReceiver::~UdpReceiver() {
    close();
#ifdef _WIN32
    if (wsa_initialized_) WSACleanup();
#endif
}

bool UdpReceiver::bind(const std::string& address, uint16_t port) {
    close();

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kInvalidSock) return false;

    int reuse = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (address.empty() || address == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, address.c_str(), &addr.sin_addr);
    }

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        close();
        return false;
    }

    return true;
}

bool UdpReceiver::join_multicast(const std::string& group) {
    if (sock_ == kInvalidSock) return false;

    struct ip_mreq mreq{};
    inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;

    return setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
}

int UdpReceiver::recv(void* buf, size_t buf_size, int timeout_ms) {
    if (sock_ == kInvalidSock) return -1;

    if (timeout_ms >= 0) {
#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(timeout_ms);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        struct pollfd pfd{};
        pfd.fd = sock_;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return ret;
#endif
    }

    int n = ::recvfrom(sock_, static_cast<char*>(buf), static_cast<int>(buf_size),
                       0, nullptr, nullptr);
    return n;
}

void UdpReceiver::close() {
    if (sock_ != kInvalidSock) {
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = kInvalidSock;
    }
}

bool UdpReceiver::is_open() const {
    return sock_ != kInvalidSock;
}

// Factory
std::unique_ptr<INetworkReceiver> create_network_receiver(NetworkReceiverType type) {
    switch (type) {
    case NetworkReceiverType::UDP:
        return std::make_unique<UdpReceiver>();
    case NetworkReceiverType::XDP:
    case NetworkReceiverType::DPDK:
        return nullptr;
    }
    return nullptr;
}

} // namespace hft
