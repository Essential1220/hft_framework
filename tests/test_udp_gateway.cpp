// ============================================
// test_udp_gateway.cpp - UDP multicast publisher/receiver round-trip tests
// Uses loopback (127.0.0.1) instead of real multicast for portability.
// ============================================

#include "gateway/udp_md_publisher.h"
#include "common/types.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define TEST_ASSERT(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            throw std::runtime_error(std::string("ASSERT FAILED: ") + #cond +  \
                                     " at " + __FILE__ + ":" +                 \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_EQ(a, b)                                                  \
    do {                                                                       \
        if ((a) != (b)) {                                                      \
            throw std::runtime_error(std::string("ASSERT_EQ FAILED: ") +       \
                                     #a " != " #b " at " + __FILE__ + ":" +   \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_NEAR(a, b, eps)                                           \
    do {                                                                       \
        double _diff = (double)(a) - (double)(b);                             \
        if (_diff < 0) _diff = -_diff;                                        \
        if (_diff > (eps)) {                                                   \
            throw std::runtime_error(std::string("ASSERT_NEAR FAILED: ") +     \
                                     #a " != " #b " (diff=" +                  \
                                     std::to_string(_diff) + ") at " +         \
                                     __FILE__ + ":" + std::to_string(__LINE__));\
        }                                                                      \
    } while (0)

void test_udp_publish_receive_roundtrip() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    // Set up receiver on loopback
    auto recv_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    TEST_ASSERT(recv_sock != static_cast<decltype(recv_sock)>(-1));

    int reuse = 1;
    setsockopt(recv_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(19999);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    TEST_ASSERT(::bind(recv_sock, reinterpret_cast<struct sockaddr*>(&bind_addr),
                       sizeof(bind_addr)) == 0);

    // Publisher sends to loopback unicast (not real multicast, to avoid network config)
    hft::UdpMdPublisher pub;
    TEST_ASSERT(pub.init("127.0.0.1", 19999, 0));

    hft::TickData tick{};
    hft::safe_copy(tick.instrument_id, "IF2506", sizeof(tick.instrument_id));
    tick.last_price = 4567.8;
    tick.volume = 12345;
    tick.bid[0].price = 4567.6;
    tick.bid[0].volume = 100;
    tick.ask[0].price = 4568.0;
    tick.ask[0].volume = 200;

    TEST_ASSERT(pub.publish(tick));
    TEST_ASSERT_EQ(pub.sequence(), 1u);

    // Receive
    constexpr size_t kBufSize = sizeof(hft::UdpTickHeader) + sizeof(hft::TickData);
    char buf[kBufSize];

#ifdef _WIN32
    DWORD timeout_val = 2000;
    setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_val), sizeof(timeout_val));
#else
    struct timeval tv{};
    tv.tv_sec = 2;
    setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    int n = recvfrom(recv_sock, buf, sizeof(buf), 0, nullptr, nullptr);
    TEST_ASSERT(n == static_cast<int>(kBufSize));

    hft::UdpTickHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));
    TEST_ASSERT(std::memcmp(hdr.magic, hft::kUdpTickMagic, 4) == 0);
    TEST_ASSERT_EQ(hdr.sequence, 0u);
    TEST_ASSERT_EQ(hdr.payload_size, static_cast<uint32_t>(sizeof(hft::TickData)));

    hft::TickData received{};
    std::memcpy(&received, buf + sizeof(hdr), sizeof(received));
    TEST_ASSERT(std::strcmp(received.instrument_id, "IF2506") == 0);
    TEST_ASSERT_NEAR(received.last_price, 4567.8, 0.01);
    TEST_ASSERT_EQ(received.volume, 12345);
    TEST_ASSERT_EQ(received.bid[0].volume, 100);
    TEST_ASSERT_EQ(received.ask[0].volume, 200);

    pub.close();
#ifdef _WIN32
    closesocket(recv_sock);
    WSACleanup();
#else
    ::close(recv_sock);
#endif
}

void test_udp_header_format() {
    TEST_ASSERT_EQ(sizeof(hft::UdpTickHeader), size_t(24));
    hft::UdpTickHeader hdr{};
    std::memcpy(hdr.magic, hft::kUdpTickMagic, 4);
    TEST_ASSERT(hdr.magic[0] == 'H');
    TEST_ASSERT(hdr.magic[1] == 'F');
    TEST_ASSERT(hdr.magic[2] == 'T');
    TEST_ASSERT(hdr.magic[3] == 'U');
}
