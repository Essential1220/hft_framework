// ============================================
// qdp_trade_gateway.cpp — QDP(上期所 FTD)交易网关实现
// 与 ctp_trade_gateway.cpp 几乎对称:
//   - 类前缀 CThostFtdc -> CFtdc
//   - 取消 ReqAuthenticate 一步,OnFrontConnected 直接 req_login()
//   - 协议枚举值 (THOST_FTDC_*) 保留:QDP/CTP 同源,值一致;
//     如未来用户接入的 QDP SDK 改用 FTDC_* 前缀,定位到此文件全局替换即可。
// ============================================

#include "gateway/qdp_trade_gateway.h"

#ifdef HFT_HAS_QDP

#include "common/crypto.h"
#include "common/encoding.h"
#include "common/logger.h"
#include "engine/trading_engine.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace hft {

namespace {

std::string make_position_key(const char* instrument, Direction dir) {
    return std::string(instrument) + (dir == Direction::Buy ? "_L" : "_S");
}

std::string qdp_error_text(CFtdcRspInfoField* pRspInfo) {
    if (!pRspInfo) {
        return "QDP: unknown error";
    }
    return "[" + std::to_string(pRspInfo->ErrorID) + "] " + gbk_to_utf8(pRspInfo->ErrorMsg);
}

} // namespace

void QdpTradeGateway::throttle_query() {
    std::lock_guard<std::mutex> lock(query_throttle_mtx_);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_query_time_).count();
    if (elapsed < kQueryIntervalMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kQueryIntervalMs - elapsed));
    }
    last_query_time_ = std::chrono::steady_clock::now();
}

QdpTradeGateway::~QdpTradeGateway() {
    stop();
}

void QdpTradeGateway::init(const Config& config, const std::string& section,
                           TradingEngine* engine, const std::string& account_id) {
    engine_ = engine;
    account_id_ = account_id;
    broker_id_ = config.get_string(section, "BrokerID");
    user_id_ = config.get_string(section, "UserID");
    password_ = crypto::decrypt_config_value(config.get_string(section, "Password"));
    std::string front = config.get_string(section, "TradeFront");
    if (front.empty()) {
        LOG_ERROR("QDP 交易网关前置地址为空, section=" + section);
        login_failed_ = true;
        return;
    }
    logged_in_ = false;
    login_failed_ = false;

    std::string flow_dir = "td_flow_" + account_id + "//";
    std::filesystem::create_directories("td_flow_" + account_id);

    api_ = CFtdcTraderApi::CreateFtdcTraderApi(flow_dir.c_str());
    if (!api_) {
        LOG_ERROR("QDP 交易网关创建 API 失败, section=" + section);
        login_failed_ = true;
        return;
    }
    api_->RegisterSpi(this);
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);
    api_->RegisterFront(const_cast<char*>(front.c_str()));

    LOG_INFO("QDP 交易网关连接前置: " + front);
    api_->Init();
}

void QdpTradeGateway::stop() {
    if (api_) {
        api_->RegisterSpi(nullptr);
        api_->Release();
        api_ = nullptr;
        logged_in_ = false;
        login_failed_ = true;
        login_cv_.notify_all();
        LOG_INFO("QDP trade gateway stopped");
    }
}

bool QdpTradeGateway::wait_for_login(int timeout_sec) {
    std::unique_lock<std::mutex> lock(login_mtx_);
    return login_cv_.wait_for(lock, std::chrono::seconds(timeout_sec),
                              [this] { return logged_in_.load() || login_failed_.load(); }) &&
           logged_in_.load();
}

int QdpTradeGateway::send_order(const OrderRequest& req, const std::string& order_ref) {
    if (!api_) return -1;
    CFtdcInputOrderField field{};

    std::strncpy(field.BrokerID, broker_id_.c_str(), sizeof(field.BrokerID) - 1);
    std::strncpy(field.InvestorID, user_id_.c_str(), sizeof(field.InvestorID) - 1);
    std::strncpy(field.UserID, user_id_.c_str(), sizeof(field.UserID) - 1);
    std::strncpy(field.InstrumentID, req.instrument_id, sizeof(field.InstrumentID) - 1);
    std::strncpy(field.ExchangeID, req.exchange_id, sizeof(field.ExchangeID) - 1);
    std::strncpy(field.OrderRef, order_ref.c_str(), sizeof(field.OrderRef) - 1);

    switch (req.price_type) {
        case OrderRequest::PriceType::Market:
            field.OrderPriceType = THOST_FTDC_OPT_AnyPrice;
            field.TimeCondition = THOST_FTDC_TC_IOC;
            field.VolumeCondition = THOST_FTDC_VC_AV;
            field.LimitPrice = 0.0;
            break;
        case OrderRequest::PriceType::Fak:
            field.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
            field.TimeCondition = THOST_FTDC_TC_IOC;
            field.VolumeCondition = THOST_FTDC_VC_AV;
            field.LimitPrice = req.price;
            break;
        case OrderRequest::PriceType::Limit:
        default:
            field.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
            field.TimeCondition = THOST_FTDC_TC_GFD;
            field.VolumeCondition = THOST_FTDC_VC_AV;
            field.LimitPrice = req.price;
            break;
    }
    field.VolumeTotalOriginal = req.volume;
    field.Direction = (req.direction == Direction::Buy) ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;

    switch (req.offset) {
        case Offset::Open:           field.CombOffsetFlag[0] = THOST_FTDC_OF_Open; break;
        case Offset::Close:          field.CombOffsetFlag[0] = THOST_FTDC_OF_Close; break;
        case Offset::CloseToday:     field.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday; break;
        case Offset::CloseYesterday: field.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday; break;
        default:
            LOG_ERROR("QDP send_order: unknown offset value=" + std::to_string(static_cast<int>(req.offset)) +
                      " ref=" + order_ref);
            return -1;
    }

    field.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    if (req.price_type != OrderRequest::PriceType::Market &&
        req.price_type != OrderRequest::PriceType::Fak) {
        field.TimeCondition = THOST_FTDC_TC_GFD;
        field.VolumeCondition = THOST_FTDC_VC_AV;
    }
    field.ContingentCondition = THOST_FTDC_CC_Immediately;
    field.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    field.MinVolume = 1;
    field.IsAutoSuspend = 0;
    field.UserForceClose = 0;

    const int request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int ret = api_->ReqOrderInsert(&field, request_id);
    if (ret != 0) {
        LOG_ERROR("QDP 交易网关报单失败, ret=" + std::to_string(ret) +
                  " ref=" + order_ref);
    } else {
        LOG_INFO("QDP 交易网关报单已发送 ref=" + order_ref +
                 " instrument=" + std::string(req.instrument_id) +
                 " price=" + std::to_string(req.price) +
                 " volume=" + std::to_string(req.volume));
    }
    return ret;
}

int QdpTradeGateway::cancel_order(const std::string& instrument_id, const std::string& exchange_id,
                                  const std::string& order_ref, int front_id, int session_id) {
    if (!api_) return -1;
    CFtdcInputOrderActionField field{};
    std::strncpy(field.BrokerID, broker_id_.c_str(), sizeof(field.BrokerID) - 1);
    std::strncpy(field.InvestorID, user_id_.c_str(), sizeof(field.InvestorID) - 1);
    std::strncpy(field.UserID, user_id_.c_str(), sizeof(field.UserID) - 1);
    std::strncpy(field.InstrumentID, instrument_id.c_str(), sizeof(field.InstrumentID) - 1);
    std::strncpy(field.ExchangeID, exchange_id.c_str(), sizeof(field.ExchangeID) - 1);
    std::strncpy(field.OrderRef, order_ref.c_str(), sizeof(field.OrderRef) - 1);
    field.FrontID = front_id;
    field.SessionID = session_id;
    field.ActionFlag = THOST_FTDC_AF_Delete;

    const int request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int ret = api_->ReqOrderAction(&field, request_id);
    if (ret != 0) {
        LOG_ERROR("QDP 交易网关撤单失败, ret=" + std::to_string(ret) +
                  " ref=" + order_ref);
    } else {
        LOG_INFO("QDP 交易网关撤单已发送 ref=" + order_ref);
    }
    return ret;
}

int QdpTradeGateway::query_account() {
    throttle_query();
    CFtdcQryTradingAccountField field{};
    std::strncpy(field.BrokerID, broker_id_.c_str(), sizeof(field.BrokerID) - 1);
    std::strncpy(field.InvestorID, user_id_.c_str(), sizeof(field.InvestorID) - 1);
    const int request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    return api_->ReqQryTradingAccount(&field, request_id);
}

int QdpTradeGateway::query_position(const std::string& instrument_id) {
    throttle_query();
    const int request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> lock(pending_positions_mtx_);
        pending_positions_.clear();
        pending_positions_request_id_ = request_id;
    }

    CFtdcQryInvestorPositionField field{};
    std::strncpy(field.BrokerID, broker_id_.c_str(), sizeof(field.BrokerID) - 1);
    std::strncpy(field.InvestorID, user_id_.c_str(), sizeof(field.InvestorID) - 1);
    if (!instrument_id.empty()) {
        std::strncpy(field.InstrumentID, instrument_id.c_str(), sizeof(field.InstrumentID) - 1);
    }
    return api_->ReqQryInvestorPosition(&field, request_id);
}

int QdpTradeGateway::query_active_orders() {
    throttle_query();
    const int request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> lock(pending_orders_mtx_);
        pending_active_orders_.clear();
        pending_active_orders_request_id_ = request_id;
    }

    CFtdcQryOrderField field{};
    std::strncpy(field.BrokerID, broker_id_.c_str(), sizeof(field.BrokerID) - 1);
    std::strncpy(field.InvestorID, user_id_.c_str(), sizeof(field.InvestorID) - 1);
    return api_->ReqQryOrder(&field, request_id);
}

int QdpTradeGateway::query_instrument_rates(const std::string& instrument_id, const std::string& exchange_id) {
    if (!api_ || !logged_in_) {
        return -1;
    }
    if (instrument_id.empty()) {
        return -2;
    }
    throttle_query();

    CFtdcQryInstrumentMarginRateField margin_field{};
    std::strncpy(margin_field.BrokerID, broker_id_.c_str(), sizeof(margin_field.BrokerID) - 1);
    std::strncpy(margin_field.InvestorID, user_id_.c_str(), sizeof(margin_field.InvestorID) - 1);
    std::strncpy(margin_field.InstrumentID, instrument_id.c_str(), sizeof(margin_field.InstrumentID) - 1);
    if (!exchange_id.empty()) {
        std::strncpy(margin_field.ExchangeID, exchange_id.c_str(), sizeof(margin_field.ExchangeID) - 1);
    }
    margin_field.HedgeFlag = THOST_FTDC_HF_Speculation;
    const int margin_request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int margin_ret = api_->ReqQryInstrumentMarginRate(&margin_field, margin_request_id);
    if (margin_ret != 0) {
        LOG_WARN("QDP query margin rate failed ret=" + std::to_string(margin_ret) + " instrument=" + instrument_id);
    }

    throttle_query();

    CFtdcQryInstrumentCommissionRateField commission_field{};
    std::strncpy(commission_field.BrokerID, broker_id_.c_str(), sizeof(commission_field.BrokerID) - 1);
    std::strncpy(commission_field.InvestorID, user_id_.c_str(), sizeof(commission_field.InvestorID) - 1);
    std::strncpy(commission_field.InstrumentID, instrument_id.c_str(), sizeof(commission_field.InstrumentID) - 1);
    if (!exchange_id.empty()) {
        std::strncpy(commission_field.ExchangeID, exchange_id.c_str(), sizeof(commission_field.ExchangeID) - 1);
    }
    const int commission_request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int commission_ret = api_->ReqQryInstrumentCommissionRate(&commission_field, commission_request_id);
    if (commission_ret != 0) {
        LOG_WARN("QDP query commission rate failed ret=" + std::to_string(commission_ret) + " instrument=" + instrument_id);
    }

    return margin_ret != 0 ? margin_ret : commission_ret;
}

std::vector<std::string> QdpTradeGateway::query_instruments(int timeout_sec) {
    if (!api_ || !logged_in_) {
        LOG_WARN("QDP 交易网关查询合约列表失败：网关未登录，account=" + account_id_);
        return {};
    }
    throttle_query();

    const int request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> lock(pending_instruments_mtx_);
        pending_instruments_request_id_ = request_id;
        pending_instruments_.clear();
        pending_instruments_done_ = false;
    }

    CFtdcQryInstrumentField field{};
    const int ret = api_->ReqQryInstrument(&field, request_id);
    if (ret != 0) {
        LOG_ERROR("QDP 交易网关查询合约列表请求失败, ret=" + std::to_string(ret) + " account=" + account_id_);
        std::lock_guard<std::mutex> lock(pending_instruments_mtx_);
        pending_instruments_done_ = true;
        pending_instruments_cv_.notify_all();
        return {};
    }

    std::unique_lock<std::mutex> lock(pending_instruments_mtx_);
    const bool ready = pending_instruments_cv_.wait_for(
        lock,
        std::chrono::seconds(timeout_sec),
        [this, request_id] { return pending_instruments_done_ && pending_instruments_request_id_ == request_id; });

    if (!ready) {
        LOG_WARN("QDP 交易网关查询合约列表超时, account=" + account_id_);
        return pending_instruments_;
    }
    return pending_instruments_;
}

void QdpTradeGateway::OnFrontConnected() {
    // QDP 无 AuthCode/AppID,直接登录
    LOG_INFO("QDP trade gateway front connected, logging in");
    req_login();
}

void QdpTradeGateway::OnFrontDisconnected(int nReason) {
    logged_in_ = false;
    char hex_buf[16];
    std::snprintf(hex_buf, sizeof(hex_buf), "0x%04X", nReason);
    LOG_WARN("QDP 交易网关已断开, reason=" + std::string(hex_buf) + " account=" + account_id_);

    if (engine_) {
        engine_->on_gateway_error(account_id_, OrderRejectReason::GatewayDisconnected,
                                  "QDP trade front disconnected reason=" + std::string(hex_buf));
    }
}

void QdpTradeGateway::OnRspUserLogin(CFtdcRspUserLoginField* pRspUserLogin,
                                     CFtdcRspInfoField* pRspInfo,
                                     int, bool) {
    if (is_error_rsp(pRspInfo)) {
        report_gateway_error(OrderRejectReason::GatewayLoginFailed,
                             "QDP trade login failed " + qdp_error_text(pRspInfo));
        LOG_ERROR("QDP trade gateway login failed");
        return;
    }

    if (!pRspUserLogin) {
        report_gateway_error(OrderRejectReason::GatewayLoginFailed,
                             "QDP trade login returned null response");
        LOG_ERROR("QDP trade gateway login failed: pRspUserLogin is null");
        return;
    }

    front_id_ = pRspUserLogin->FrontID;
    session_id_ = pRspUserLogin->SessionID;
    max_order_ref_ = std::atoi(pRspUserLogin->MaxOrderRef);

    LOG_INFO("QDP 交易网关登录成功: front_id=" + std::to_string(front_id_) +
             " session_id=" + std::to_string(session_id_) +
             " max_order_ref=" + std::to_string(max_order_ref_) +
             " trading_day=" + std::string(pRspUserLogin->TradingDay));

    if (engine_) {
        engine_->on_trading_day(pRspUserLogin->TradingDay);
    }
    req_settlement_confirm();
}

void QdpTradeGateway::OnRspSettlementInfoConfirm(CFtdcSettlementInfoConfirmField*,
                                                 CFtdcRspInfoField* pRspInfo,
                                                 int, bool) {
    if (is_error_rsp(pRspInfo)) {
        report_gateway_error(OrderRejectReason::GatewayLoginFailed,
                             "QDP settlement confirm failed " + qdp_error_text(pRspInfo));
        LOG_ERROR("QDP trade gateway settlement confirm failed");
        return;
    }

    logged_in_ = true;
    login_cv_.notify_all();

    if (!initial_login_done_) {
        initial_login_done_ = true;
        LOG_INFO("QDP 交易网关结算确认完成，交易已就绪");
    } else {
        LOG_INFO("QDP 交易网关重登录完成 account=" + account_id_);
        if (engine_) {
            engine_->on_trade_reconnected(account_id_, front_id_, session_id_, max_order_ref_);
        }
    }
}

void QdpTradeGateway::OnRspOrderInsert(CFtdcInputOrderField* pInputOrder,
                                       CFtdcRspInfoField* pRspInfo,
                                       int, bool) {
    if (is_error_rsp(pRspInfo) && pInputOrder) {
        OrderInfo info{};
        safe_copy(info.instrument_id, pInputOrder->InstrumentID, sizeof(info.instrument_id));
        safe_copy(info.exchange_id, pInputOrder->ExchangeID, sizeof(info.exchange_id));
        safe_copy(info.order_ref, pInputOrder->OrderRef, sizeof(info.order_ref));
        safe_copy(info.account_id, account_id_.c_str(), sizeof(info.account_id));
        info.status = OrderStatus::Error;
        safe_copy(info.status_msg,
                  gbk_to_utf8(pRspInfo ? pRspInfo->ErrorMsg : "Unknown error").c_str(),
                  sizeof(info.status_msg));
        if (engine_) {
            engine_->on_order(info);
        }
    }
}

void QdpTradeGateway::OnRspOrderAction(CFtdcInputOrderActionField* pInputOrderAction,
                                       CFtdcRspInfoField* pRspInfo,
                                       int, bool) {
    if (is_error_rsp(pRspInfo) && pInputOrderAction) {
        const std::string reason = gbk_to_utf8(pRspInfo ? pRspInfo->ErrorMsg : "Unknown error");
        LOG_ERROR("QDP 交易网关撤单被拒绝 ref=" + std::string(pInputOrderAction->OrderRef) +
                  " account=" + account_id_ +
                  " reason=" + reason);
        if (engine_) {
            engine_->on_cancel_rejected(account_id_, pInputOrderAction->OrderRef, reason);
        }
    }
}

void QdpTradeGateway::OnRtnOrder(CFtdcOrderField* pOrder) {
    if (!pOrder || !engine_) return;

    OrderInfo info{};
    safe_copy(info.instrument_id, pOrder->InstrumentID, sizeof(info.instrument_id));
    safe_copy(info.exchange_id, pOrder->ExchangeID, sizeof(info.exchange_id));
    safe_copy(info.order_ref, pOrder->OrderRef, sizeof(info.order_ref));
    safe_copy(info.order_sys_id, pOrder->OrderSysID, sizeof(info.order_sys_id));
    info.price = pOrder->LimitPrice;
    info.total_volume = pOrder->VolumeTotalOriginal;
    info.traded_volume = pOrder->VolumeTraded;
    info.front_id = pOrder->FrontID;
    info.session_id = pOrder->SessionID;
    safe_copy(info.insert_time, pOrder->InsertTime, sizeof(info.insert_time));
    safe_copy(info.status_msg, gbk_to_utf8(pOrder->StatusMsg).c_str(), sizeof(info.status_msg));
    info.direction = (pOrder->Direction == THOST_FTDC_D_Buy) ? Direction::Buy : Direction::Sell;
    safe_copy(info.account_id, account_id_.c_str(), sizeof(info.account_id));

    switch (pOrder->CombOffsetFlag[0]) {
        case THOST_FTDC_OF_Open:           info.offset = Offset::Open; break;
        case THOST_FTDC_OF_Close:          info.offset = Offset::Close; break;
        case THOST_FTDC_OF_CloseToday:     info.offset = Offset::CloseToday; break;
        case THOST_FTDC_OF_CloseYesterday: info.offset = Offset::CloseYesterday; break;
        default:                           info.offset = Offset::Close; break;
    }

    switch (pOrder->OrderStatus) {
        case THOST_FTDC_OST_AllTraded:              info.status = OrderStatus::AllTraded; break;
        case THOST_FTDC_OST_PartTradedQueueing:     info.status = OrderStatus::PartTraded; break;
        case THOST_FTDC_OST_PartTradedNotQueueing:  info.status = OrderStatus::Cancelled; break;
        case THOST_FTDC_OST_NoTradeQueueing:        info.status = OrderStatus::Pending; break;
        case THOST_FTDC_OST_NoTradeNotQueueing:     info.status = OrderStatus::Cancelled; break;
        case THOST_FTDC_OST_Canceled:               info.status = OrderStatus::Cancelled; break;
        default:
            info.status = OrderStatus::Error;
            LOG_WARN("unmapped QDP OrderStatus=" + std::to_string(static_cast<int>(pOrder->OrderStatus)) +
                     " ref=" + std::string(pOrder->OrderRef));
            break;
    }

    engine_->on_order(info);
}

void QdpTradeGateway::OnRtnTrade(CFtdcTradeField* pTrade) {
    if (!pTrade || !engine_) return;

    TradeInfo info{};
    safe_copy(info.instrument_id, pTrade->InstrumentID, sizeof(info.instrument_id));
    safe_copy(info.exchange_id, pTrade->ExchangeID, sizeof(info.exchange_id));
    safe_copy(info.trade_id, pTrade->TradeID, sizeof(info.trade_id));
    safe_copy(info.order_ref, pTrade->OrderRef, sizeof(info.order_ref));
    info.price = pTrade->Price;
    info.volume = pTrade->Volume;
    safe_copy(info.trade_time, pTrade->TradeTime, sizeof(info.trade_time));
    info.direction = (pTrade->Direction == THOST_FTDC_D_Buy) ? Direction::Buy : Direction::Sell;
    safe_copy(info.account_id, account_id_.c_str(), sizeof(info.account_id));

    switch (pTrade->OffsetFlag) {
        case THOST_FTDC_OF_Open:           info.offset = Offset::Open; break;
        case THOST_FTDC_OF_Close:          info.offset = Offset::Close; break;
        case THOST_FTDC_OF_CloseToday:     info.offset = Offset::CloseToday; break;
        case THOST_FTDC_OF_CloseYesterday: info.offset = Offset::CloseYesterday; break;
        default:                           info.offset = Offset::Close; break;
    }

    engine_->on_trade(info);
}

void QdpTradeGateway::OnRspQryTradingAccount(CFtdcTradingAccountField* pTradingAccount,
                                             CFtdcRspInfoField* pRspInfo,
                                             int, bool bIsLast) {
    if (is_error_rsp(pRspInfo)) return;
    if (!engine_ || !bIsLast) return;

    AccountInfo account{};
    safe_copy(account.account_id, account_id_.c_str(), sizeof(account.account_id));
    if (pTradingAccount) {
        account.balance = pTradingAccount->Balance;
        account.available = pTradingAccount->Available;
        account.margin = pTradingAccount->CurrMargin;
        account.commission = pTradingAccount->Commission;
        account.close_profit = pTradingAccount->CloseProfit;
        account.position_profit = pTradingAccount->PositionProfit;
        account.frozen_margin = pTradingAccount->FrozenMargin;
        account.frozen_commission = pTradingAccount->FrozenCommission;
    }

    engine_->apply_account_snapshot(account);
}

void QdpTradeGateway::OnRspQryInvestorPosition(CFtdcInvestorPositionField* pPos,
                                               CFtdcRspInfoField* pRspInfo,
                                               int nRequestID, bool bIsLast) {
    if (is_error_rsp(pRspInfo)) return;
    if (!engine_) return;

    if (pPos) {
        PendingPositionSnapshot snapshot{};
        const Direction dir = (pPos->PosiDirection == THOST_FTDC_PD_Long) ? Direction::Buy : Direction::Sell;
        const std::string key = make_position_key(pPos->InstrumentID, dir);

        std::lock_guard<std::mutex> lock(pending_positions_mtx_);
        if (nRequestID != pending_positions_request_id_) {
            return;
        }
        auto& agg = pending_positions_[key];
        safe_copy(agg.pos.instrument_id, pPos->InstrumentID, sizeof(agg.pos.instrument_id));
        safe_copy(agg.pos.account_id, account_id_.c_str(), sizeof(agg.pos.account_id));
        agg.pos.direction = dir;
        agg.pos.total += pPos->Position;
        agg.pos.today += pPos->TodayPosition;
        agg.pos.yesterday += pPos->YdPosition;
        agg.pos.position_profit += pPos->PositionProfit;
        agg.pos.use_margin += pPos->UseMargin;
        agg.total_cost += pPos->PositionCost;
    }

    if (!bIsLast) return;

    std::vector<PositionInfo> snapshot;
    {
        std::lock_guard<std::mutex> lock(pending_positions_mtx_);
        if (nRequestID != pending_positions_request_id_) {
            return;
        }
        snapshot.reserve(pending_positions_.size());
        for (auto& [key, agg] : pending_positions_) {
            if (agg.pos.total > 0) {
                agg.pos.avg_price = agg.total_cost / agg.pos.total;
                snapshot.push_back(agg.pos);
            }
        }
        pending_positions_.clear();
        pending_positions_request_id_ = 0;
    }

    engine_->apply_position_snapshot(account_id_, snapshot);
}

void QdpTradeGateway::OnRspQryOrder(CFtdcOrderField* pOrder,
                                    CFtdcRspInfoField* pRspInfo,
                                    int nRequestID, bool bIsLast) {
    if (is_error_rsp(pRspInfo)) return;
    if (!engine_) return;

    std::vector<OrderInfo> snapshot;
    if (pOrder) {
        OrderInfo info{};
        safe_copy(info.instrument_id, pOrder->InstrumentID, sizeof(info.instrument_id));
        safe_copy(info.exchange_id, pOrder->ExchangeID, sizeof(info.exchange_id));
        safe_copy(info.account_id, account_id_.c_str(), sizeof(info.account_id));
        safe_copy(info.order_ref, pOrder->OrderRef, sizeof(info.order_ref));
        safe_copy(info.order_sys_id, pOrder->OrderSysID, sizeof(info.order_sys_id));
        safe_copy(info.insert_time, pOrder->InsertTime, sizeof(info.insert_time));
        safe_copy(info.status_msg, gbk_to_utf8(pOrder->StatusMsg).c_str(), sizeof(info.status_msg));
        info.price = pOrder->LimitPrice;
        info.total_volume = pOrder->VolumeTotalOriginal;
        info.traded_volume = pOrder->VolumeTraded;
        info.front_id = pOrder->FrontID;
        info.session_id = pOrder->SessionID;
        info.direction = (pOrder->Direction == THOST_FTDC_D_Buy) ? Direction::Buy : Direction::Sell;

        switch (pOrder->CombOffsetFlag[0]) {
            case THOST_FTDC_OF_Open:           info.offset = Offset::Open; break;
            case THOST_FTDC_OF_Close:          info.offset = Offset::Close; break;
            case THOST_FTDC_OF_CloseToday:     info.offset = Offset::CloseToday; break;
            case THOST_FTDC_OF_CloseYesterday: info.offset = Offset::CloseYesterday; break;
            default:                           info.offset = Offset::Close; break;
        }

        switch (pOrder->OrderStatus) {
            case THOST_FTDC_OST_AllTraded:              info.status = OrderStatus::AllTraded; break;
            case THOST_FTDC_OST_PartTradedQueueing:     info.status = OrderStatus::PartTraded; break;
            case THOST_FTDC_OST_PartTradedNotQueueing:  info.status = OrderStatus::Cancelled; break;
            case THOST_FTDC_OST_NoTradeQueueing:        info.status = OrderStatus::Pending; break;
            case THOST_FTDC_OST_NoTradeNotQueueing:     info.status = OrderStatus::Cancelled; break;
            case THOST_FTDC_OST_Canceled:               info.status = OrderStatus::Cancelled; break;
            default:
                info.status = OrderStatus::Error;
                LOG_WARN("unmapped QDP OrderStatus in QryOrder: " +
                         std::to_string(static_cast<int>(pOrder->OrderStatus)) +
                         " ref=" + std::string(pOrder->OrderRef));
                break;
        }

        if (info.status == OrderStatus::Pending || info.status == OrderStatus::PartTraded) {
            std::lock_guard<std::mutex> lock(pending_orders_mtx_);
            if (nRequestID != pending_active_orders_request_id_) {
                return;
            }
            pending_active_orders_.push_back(info);
        }
    }

    if (!bIsLast) return;

    {
        std::lock_guard<std::mutex> lock(pending_orders_mtx_);
        if (nRequestID != pending_active_orders_request_id_) {
            return;
        }
        snapshot = pending_active_orders_;
        pending_active_orders_.clear();
        pending_active_orders_request_id_ = 0;
    }

    engine_->apply_active_orders_snapshot(account_id_, snapshot);
}

void QdpTradeGateway::OnRspError(CFtdcRspInfoField* pRspInfo, int, bool) {
    is_error_rsp(pRspInfo);
}

void QdpTradeGateway::OnRspQryInstrumentMarginRate(CFtdcInstrumentMarginRateField* pRate,
                                                   CFtdcRspInfoField* pRspInfo,
                                                   int, bool) {
    if (is_error_rsp(pRspInfo) || !pRate || !engine_) {
        return;
    }
    InstrumentSpec spec;
    spec.instrument_id = pRate->InstrumentID;
    spec.exchange_id = pRate->ExchangeID;
    if (spec.exchange_id.empty()) spec.exchange_id = get_exchange_id(spec.instrument_id.c_str());
    spec.product_id = "";
    const auto current = engine_->get_instrument_specs(spec.instrument_id);
    if (!current.empty()) {
        spec = current.front();
    }
    if (pRate->LongMarginRatioByMoney > 0.0) spec.long_margin_ratio = pRate->LongMarginRatioByMoney;
    if (pRate->ShortMarginRatioByMoney > 0.0) spec.short_margin_ratio = pRate->ShortMarginRatioByMoney;
    if (spec.exchange_id.empty()) spec.exchange_id = pRate->ExchangeID;
    engine_->on_instrument_spec_update(spec);
    LOG_INFO("QDP instrument margin rate updated: " + spec.instrument_id);
}

void QdpTradeGateway::OnRspQryInstrumentCommissionRate(CFtdcInstrumentCommissionRateField* pRate,
                                                       CFtdcRspInfoField* pRspInfo,
                                                       int, bool) {
    if (is_error_rsp(pRspInfo) || !pRate || !engine_) {
        return;
    }
    InstrumentSpec spec;
    spec.instrument_id = pRate->InstrumentID;
    spec.exchange_id = pRate->ExchangeID;
    if (spec.exchange_id.empty()) spec.exchange_id = get_exchange_id(spec.instrument_id.c_str());
    const auto current = engine_->get_instrument_specs(spec.instrument_id);
    if (!current.empty()) {
        spec = current.front();
    }
    if (pRate->OpenRatioByMoney > 0.0) spec.open_commission_rate = pRate->OpenRatioByMoney;
    if (pRate->CloseRatioByMoney > 0.0) spec.close_commission_rate = pRate->CloseRatioByMoney;
    if (pRate->CloseTodayRatioByMoney > 0.0) spec.close_today_commission_rate = pRate->CloseTodayRatioByMoney;
    if (spec.exchange_id.empty()) spec.exchange_id = pRate->ExchangeID;
    engine_->on_instrument_spec_update(spec);
    LOG_INFO("QDP instrument commission rate updated: " + spec.instrument_id);
}

void QdpTradeGateway::OnRspQryInstrument(CFtdcInstrumentField* pInstrument,
                                         CFtdcRspInfoField* pRspInfo,
                                         int nRequestID, bool bIsLast) {
    {
        std::lock_guard<std::mutex> lock(pending_instruments_mtx_);
        if (nRequestID != pending_instruments_request_id_) {
            return;
        }

        if (pInstrument && pInstrument->InstrumentID[0] != '\0' &&
            (pInstrument->ProductClass == THOST_FTDC_PC_Futures || pInstrument->ProductClass == THOST_FTDC_PC_Options || pInstrument->ProductClass == THOST_FTDC_PC_SpotOption)) {
            pending_instruments_.emplace_back(pInstrument->InstrumentID);
            if (engine_) {
                InstrumentSpec spec;
                spec.instrument_id = pInstrument->InstrumentID;
                spec.instrument_name = gbk_to_utf8(pInstrument->InstrumentName);
                spec.exchange_id = pInstrument->ExchangeID;
                spec.product_id = pInstrument->ProductID;
                spec.underlying_instrument_id = pInstrument->UnderlyingInstrID;
                spec.expire_date = pInstrument->ExpireDate;
                spec.start_deliv_date = pInstrument->StartDelivDate;
                spec.end_deliv_date = pInstrument->EndDelivDate;
                spec.inst_life_phase = pInstrument->InstLifePhase;
                spec.is_trading = pInstrument->IsTrading != 0;
                spec.strike_price = pInstrument->StrikePrice;
                spec.product_class = pInstrument->ProductClass;
                spec.options_type = pInstrument->OptionsType;
                if (pInstrument->PriceTick > 0.0) spec.price_tick = pInstrument->PriceTick;
                if (pInstrument->VolumeMultiple > 0) spec.volume_multiple = pInstrument->VolumeMultiple;
                if (pInstrument->LongMarginRatio > 0.0) spec.long_margin_ratio = pInstrument->LongMarginRatio;
                if (pInstrument->ShortMarginRatio > 0.0) spec.short_margin_ratio = pInstrument->ShortMarginRatio;
                engine_->on_instrument_spec_update(spec);
            }
        }

        if (is_error_rsp(pRspInfo) || bIsLast) {
            pending_instruments_done_ = true;
        }
    }

    if (bIsLast || is_error_rsp(pRspInfo)) {
        pending_instruments_cv_.notify_all();
        if (!is_error_rsp(pRspInfo)) {
            LOG_INFO("QDP 交易网关合约列表查询完成, account=" + account_id_ +
                     " count=" + std::to_string(pending_instruments_.size()));
        }
    }
}

void QdpTradeGateway::req_login() {
    CFtdcReqUserLoginField field{};
    std::strncpy(field.BrokerID, broker_id_.c_str(), sizeof(field.BrokerID) - 1);
    std::strncpy(field.UserID, user_id_.c_str(), sizeof(field.UserID) - 1);
    std::strncpy(field.Password, password_.c_str(), sizeof(field.Password) - 1);

    const int request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int ret = api_->ReqUserLogin(&field, request_id);
    if (ret != 0) {
        report_gateway_error(OrderRejectReason::GatewayLoginFailed,
                             "QDP trade login request failed ret=" + std::to_string(ret));
        LOG_ERROR("QDP 交易网关发送登录请求失败 ret=" + std::to_string(ret));
    }
}

void QdpTradeGateway::req_settlement_confirm() {
    CFtdcSettlementInfoConfirmField field{};
    std::strncpy(field.BrokerID, broker_id_.c_str(), sizeof(field.BrokerID) - 1);
    std::strncpy(field.InvestorID, user_id_.c_str(), sizeof(field.InvestorID) - 1);

    const int request_id = request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int ret = api_->ReqSettlementInfoConfirm(&field, request_id);
    if (ret != 0) {
        report_gateway_error(OrderRejectReason::GatewayLoginFailed,
                             "QDP settlement confirm request failed ret=" + std::to_string(ret));
        LOG_ERROR("QDP 交易网关发送结算确认请求失败 ret=" + std::to_string(ret));
    }
}

void QdpTradeGateway::report_gateway_error(OrderRejectReason reason, const std::string& message) {
    login_failed_ = true;
    login_cv_.notify_all();
    if (engine_) {
        engine_->on_gateway_error(account_id_, reason, message);
    }
}

bool QdpTradeGateway::is_error_rsp(CFtdcRspInfoField* pRspInfo) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR("QDP 交易网关错误: [" + std::to_string(pRspInfo->ErrorID) + "] " +
                  gbk_to_utf8(pRspInfo->ErrorMsg));
        return true;
    }
    return false;
}

} // namespace hft

#endif // HFT_HAS_QDP
