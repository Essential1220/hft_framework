#pragma once
// ============================================
// udp_md_publisher.h - UDP multicast TickData publisher
// Machine A: CTP gateway -> UdpMdPublisher -> multicast group
// ============================================

#include "common/types.h"

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using udp_socket_t = SOCKET;
static constexpr udp_socket_t kUdpInvalidSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using udp_socket_t = int;
static constexpr udp_socket_t kUdpInvalidSock = -1;
#endif

namespace hft {

#pragma pack(push, 1)
struct UdpTickHeader {
    char magic[4];         // "HFTU"
    uint32_t sequence;
    int64_t timestamp_ns;
    uint32_t payload_size;
    uint32_t reserved;
};
#pragma pack(pop)

static constexpr char kUdpTickMagic[4] = {'H','F','T','U'};

class UdpMdPublisher {
public:
    ~UdpMdPublisher() { close(); }

    bool init(const std::string& multicast_group, uint16_t port, int ttl = 1) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_init_ = true;
#endif
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == kUdpInvalidSock) return false;

        setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl), sizeof(ttl));

        std::memset(&dest_, 0, sizeof(dest_));
        dest_.sin_family = AF_INET;
        dest_.sin_port = htons(port);
        inet_pton(AF_INET, multicast_group.c_str(), &dest_.sin_addr);

        return true;
    }

    bool publish(const TickData& tick) {
        if (sock_ == kUdpInvalidSock) return false;

        UdpTickHeader hdr{};
        std::memcpy(hdr.magic, kUdpTickMagic, 4);
        hdr.sequence = seq_++;
        hdr.timestamp_ns = tick.local_recv_ns;
        hdr.payload_size = static_cast<uint32_t>(sizeof(TickData));
        hdr.reserved = 0;

        char buf[sizeof(UdpTickHeader) + sizeof(TickData)];
        std::memcpy(buf, &hdr, sizeof(hdr));
        std::memcpy(buf + sizeof(hdr), &tick, sizeof(tick));

        int sent = sendto(sock_, buf, sizeof(buf), 0,
                          reinterpret_cast<struct sockaddr*>(&dest_), sizeof(dest_));
        return sent == sizeof(buf);
    }

    void close() {
        if (sock_ != kUdpInvalidSock) {
#ifdef _WIN32
            closesocket(sock_);
#else
            ::close(sock_);
#endif
            sock_ = kUdpInvalidSock;
        }
#ifdef _WIN32
        if (wsa_init_) { WSACleanup(); wsa_init_ = false; }
#endif
    }

    uint32_t sequence() const { return seq_; }

private:
    udp_socket_t sock_ = kUdpInvalidSock;
    struct sockaddr_in dest_{};
    uint32_t seq_ = 0;
#ifdef _WIN32
    bool wsa_init_ = false;
#endif
};

} // namespace hft
