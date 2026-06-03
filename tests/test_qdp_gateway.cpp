// ============================================
// test_qdp_gateway.cpp — QDP 网关编译时 smoke 测试
//
// Without a real QDP broker, only verify class instantiation, vtable alignment,
// and no-crash-on-destruct. Use broker-provided QDP test environment for real integration.
// 没有真实 QDP 柜台时, 只验证类能否实例化、虚函数表对齐、析构不崩。
// 真柜台联调请用券商提供的 QDP 测试环境。仅在 HFT_ENABLE_QDP=ON 下编译生效。
// ============================================

#ifdef HFT_HAS_QDP

#include "gateway/qdp_md_gateway.h"
#include "gateway/qdp_trade_gateway.h"
#include "gateway/i_md_gateway.h"
#include "gateway/i_trade_gateway.h"

#include <memory>
#include <stdexcept>
#include <string>

// Simple assertion macro (consistent with test_main.cpp style; this TU does not reference main.cpp macros)
// 简易断言宏 (与 test_main.cpp 风格保持一致, 本 TU 不直接引用 main.cpp 的宏)
#define QDP_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            throw std::runtime_error(std::string("ASSERT FAILED: ") + #cond +  \
                                     " at " + __FILE__ + ":" +                 \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

void test_qdp_md_gateway_can_construct() {
    // Construct/destroy only: verify SPI vtable valid, no init (avoids real SDK connection).
    // 仅做构造/析构, 确保 SPI 虚表正常, 不调用 init (避免 SDK 真连接)
    std::unique_ptr<hft::IMdGateway> gw = std::make_unique<hft::QdpMdGateway>();
    QDP_ASSERT(gw != nullptr);
    QDP_ASSERT(gw->is_logged_in() == false);
    QDP_ASSERT(gw->status() == hft::MdGatewayStatus::Disconnected);
}

void test_qdp_trade_gateway_can_construct() {
    std::unique_ptr<hft::ITradeGateway> gw = std::make_unique<hft::QdpTradeGateway>();
    QDP_ASSERT(gw != nullptr);
    QDP_ASSERT(gw->is_logged_in() == false);
    QDP_ASSERT(gw->get_front_id() == 0);
    QDP_ASSERT(gw->get_session_id() == 0);
}

#endif // HFT_HAS_QDP
