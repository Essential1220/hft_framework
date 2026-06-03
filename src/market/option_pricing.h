#pragma once
// ============================================
// option_pricing.h - Black-Scholes option pricing and Greeks (期权定价与希腊字母)
//
// Provides Black-Scholes pricing, implied volatility calculation (Newton-Raphson),
// and standard Greeks: delta, gamma, theta, vega, rho.
// 提供 Black-Scholes 定价、隐含波动率计算（牛顿迭代法）
// 和标准希腊字母：Delta、Gamma、Theta、Vega、Rho。
// ============================================

#include <string>

namespace hft {

// Option type: Call or Put (期权类型：看涨/看跌)
enum class OptionType {
    Call, // Call option (看涨期权)
    Put   // Put option (看跌期权)
};

// Input parameters for option pricing (期权定价输入参数)
struct OptionPricingInput {
    OptionType type = OptionType::Call;       // Call or Put (看涨/看跌)
    double option_price = 0.0;                // Market price of the option (期权市场价格)
    double underlying_price = 0.0;            // Current price of the underlying (标的价格)
    double strike_price = 0.0;                // Strike price (行权价)
    double years_to_expiry = 0.0;             // Time to expiry in years (距到期年数)
    double risk_free_rate = 0.0;              // Risk-free interest rate (无风险利率)
};

// Output metrics from option pricing (期权定价输出指标)
struct OptionPricingMetrics {
    bool ok = false;                          // Whether calculation succeeded (计算是否成功)
    std::string reason;                       // Failure reason if !ok (失败原因)
    double implied_volatility = 0.0;          // Implied volatility (隐含波动率 / IV)
    double delta = 0.0;                       // Delta: dPrice/dUnderlying (Delta：价格对标的偏导)
    double gamma = 0.0;                       // Gamma: dDelta/dUnderlying (Gamma：Delta对标的偏导)
    double theta = 0.0;                       // Theta: dPrice/dTime, per day (Theta：价格对时间偏导/每日)
    double vega = 0.0;                        // Vega: dPrice/dVol, per 1% vol change (Vega：价格对波动率偏导/每1%)
    double rho = 0.0;                         // Rho: dPrice/dRate, per 1% rate change (Rho：价格对利率偏导/每1%)
    double intrinsic_value = 0.0;             // Max(0, underlying - strike) for Call etc. (内在价值)
    double time_value = 0.0;                  // Option price - intrinsic value (时间价值)
    double theoretical_price = 0.0;           // BS theoretical price at solved IV (BS理论价格)
};

// Compute Black-Scholes option price (计算 Black-Scholes 期权理论价格)
double black_scholes_price(OptionType type,
                           double underlying_price,
                           double strike_price,
                           double years_to_expiry,
                           double risk_free_rate,
                           double volatility);

// Compute full option metrics: IV via Newton-Raphson + all Greeks
// 计算完整期权指标：牛顿法求隐含波动率 + 全部希腊字母
OptionPricingMetrics calculate_option_metrics(const OptionPricingInput& input);

} // namespace hft
