// ============================================
// test_network_receiver.cpp - Network receiver UDP loopback tests
// ============================================

#include "common/network_receiver.h"
#include "common/udp_receiver.h"

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

void test_udp_receiver_loopback() {
    auto receiver = hft::create_network_receiver(hft::NetworkReceiverType::UDP);
    TEST_ASSERT(receiver != nullptr);
    TEST_ASSERT(receiver->bind("127.0.0.1", 19876));
    TEST_ASSERT(receiver->is_open());

    // Send a packet to ourselves
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    auto sender = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(19876);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);

    const char* msg = "hello_hft";
    sendto(sender, msg, 9, 0, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));

    char buf[64]{};
    int n = receiver->recv(buf, sizeof(buf), 1000);
    TEST_ASSERT(n == 9);
    TEST_ASSERT(std::memcmp(buf, "hello_hft", 9) == 0);

    receiver->close();
    TEST_ASSERT(!receiver->is_open());

#ifdef _WIN32
    closesocket(sender);
    WSACleanup();
#else
    ::close(sender);
#endif
}

void test_udp_receiver_timeout() {
    auto receiver = hft::create_network_receiver(hft::NetworkReceiverType::UDP);
    TEST_ASSERT(receiver != nullptr);
    TEST_ASSERT(receiver->bind("127.0.0.1", 19877));

    int n = receiver->recv(nullptr, 0, 50);
    TEST_ASSERT(n <= 0);

    receiver->close();
}

void test_network_receiver_factory() {
    auto udp = hft::create_network_receiver(hft::NetworkReceiverType::UDP);
    TEST_ASSERT(udp != nullptr);

    auto afxdp = hft::create_network_receiver(hft::NetworkReceiverType::XDP);
    TEST_ASSERT(afxdp == nullptr);

    auto dpdk = hft::create_network_receiver(hft::NetworkReceiverType::DPDK);
    TEST_ASSERT(dpdk == nullptr);
}
