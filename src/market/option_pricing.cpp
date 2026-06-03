// ============================================
// option_pricing.cpp - Black-Scholes pricing & Greeks implementation (Black-Scholes 期权定价与希腊字母实现)
// ============================================

#include "market/option_pricing.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hft {
namespace {

// Minimum and maximum volatility bounds for IV solver (隐含波动率求解的波动率上下界)

constexpr double kMinVol = 0.0001;
constexpr double kMaxVol = 5.0;
constexpr double kSqrtTwoPi = 2.5066282746310005024;

bool finite_positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

// Standard normal probability density function (标准正态分布概率密度函数 / PDF)
double normal_pdf(double x) {
    return std::exp(-0.5 * x * x) / kSqrtTwoPi;
}

// Standard normal cumulative distribution function (标准正态分布累积分布函数 / CDF)
double normal_cdf(double x) {
    return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

// Intrinsic value of an option (期权的内在价值)
double intrinsic_value(OptionType type, double underlying_price, double strike_price) {
    if (type == OptionType::Call) {
        return (std::max)(0.0, underlying_price - strike_price);
    }
    return (std::max)(0.0, strike_price - underlying_price);
}

bool d1_d2(double underlying_price,
           double strike_price,
           double years_to_expiry,
           double risk_free_rate,
           double volatility,
           double* d1,
           double* d2) {
    if (!finite_positive(underlying_price) || !finite_positive(strike_price) ||
        !std::isfinite(years_to_expiry) || years_to_expiry < 0.0 ||
        !finite_positive(volatility)) {
        return false;
    }
    // At expiry (t -> 0), d1 and d2 approach +/- infinity depending on moneyness
    // 到期时 (t -> 0)，d1 和 d2 根据价内/价外趋近正负无穷
    if (years_to_expiry < 1e-10) {
        *d1 = (underlying_price > strike_price) ? 1e8 : -1e8;
        *d2 = *d1;
        return true;
    }
    const double sqrt_t = std::sqrt(years_to_expiry);
    const double denom = volatility * sqrt_t;
    if (!finite_positive(denom)) {
        return false;
    }
    *d1 = (std::log(underlying_price / strike_price) +
           (risk_free_rate + 0.5 * volatility * volatility) * years_to_expiry) / denom;
    *d2 = *d1 - denom;
    return std::isfinite(*d1) && std::isfinite(*d2);
}

double bs_vega(double underlying_price, double strike_price,
               double years_to_expiry, double risk_free_rate, double vol) {
    double d1 = 0.0, d2 = 0.0;
    if (!d1_d2(underlying_price, strike_price, years_to_expiry, risk_free_rate, vol, &d1, &d2)) {
        return 0.0;
    }
    return underlying_price * normal_pdf(d1) * std::sqrt(years_to_expiry);
}

bool implied_volatility(const OptionPricingInput& input, double* vol_out) {
    const double min_theoretical = black_scholes_price(input.type, input.underlying_price,
                                                       input.strike_price, input.years_to_expiry,
                                                       input.risk_free_rate, kMinVol);
    const double max_theoretical = black_scholes_price(input.type, input.underlying_price,
                                                       input.strike_price, input.years_to_expiry,
                                                       input.risk_free_rate, kMaxVol);
    if (!std::isfinite(min_theoretical) || !std::isfinite(max_theoretical) ||
        input.option_price < min_theoretical - 1e-8 ||
        input.option_price > max_theoretical + 1e-8) {
        return false;
    }

    // Brenner-Subrahmanyam initial estimate (Brenner-Subrahmanyam 初始估计)
    double vol = std::sqrt(2.0 * 3.14159265358979323846 / input.years_to_expiry) *
                 input.option_price / input.underlying_price;
    vol = std::clamp(vol, kMinVol, kMaxVol);

    // Newton-Raphson with bisection bounds (Newton-Raphson 迭代 + 二分法界约束)
    double lo = kMinVol;
    double hi = kMaxVol;
    for (int i = 0; i < 20; ++i) {
        const double price = black_scholes_price(input.type, input.underlying_price,
                                                 input.strike_price, input.years_to_expiry,
                                                 input.risk_free_rate, vol);
        if (!std::isfinite(price)) break;

        const double diff = price - input.option_price;
        if (std::abs(diff) < 1e-10) break;

        const double vega = bs_vega(input.underlying_price, input.strike_price,
                                    input.years_to_expiry, input.risk_free_rate, vol);
        if (vega < 1e-14) {
            // Vega too small for Newton step, fall back to bisection (Vega 太小无法牛顿迭代，回退到二分法)
            vol = 0.5 * (lo + hi);
        } else {
            const double new_vol = vol - diff / vega;
            if (new_vol < lo || new_vol > hi) {
                vol = 0.5 * (lo + hi);
            } else {
                vol = new_vol;
            }
        }

        // Maintain bisection bounds (维护二分法边界)
        const double p = black_scholes_price(input.type, input.underlying_price,
                                             input.strike_price, input.years_to_expiry,
                                             input.risk_free_rate, vol);
        if (std::isfinite(p)) {
            if (p > input.option_price) hi = vol; else lo = vol;
        }
    }

    *vol_out = vol;
    return std::isfinite(*vol_out) && *vol_out > 0.0;
}

} // namespace

double black_scholes_price(OptionType type,
                           double underlying_price,
                           double strike_price,
                           double years_to_expiry,
                           double risk_free_rate,
                           double volatility) {
    double d1 = 0.0;
    double d2 = 0.0;
    if (!d1_d2(underlying_price, strike_price, years_to_expiry,
               risk_free_rate, volatility, &d1, &d2)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double discounted_strike = strike_price * std::exp(-risk_free_rate * years_to_expiry);
    if (type == OptionType::Call) {
        return underlying_price * normal_cdf(d1) - discounted_strike * normal_cdf(d2);
    }
    return discounted_strike * normal_cdf(-d2) - underlying_price * normal_cdf(-d1);
}

OptionPricingMetrics calculate_option_metrics(const OptionPricingInput& input) {
    OptionPricingMetrics result;

    if (!finite_positive(input.option_price)) {
        result.reason = "invalid_option_price";
        return result;
    }
    if (!finite_positive(input.underlying_price)) {
        result.reason = "invalid_underlying_price";
        return result;
    }
    if (!finite_positive(input.strike_price)) {
        result.reason = "invalid_strike_price";
        return result;
    }
    if (!finite_positive(input.years_to_expiry)) {
        result.reason = "invalid_expiry";
        return result;
    }

    result.intrinsic_value = intrinsic_value(input.type, input.underlying_price, input.strike_price);
    result.time_value = input.option_price - result.intrinsic_value;
    if (result.time_value < -1e-8) {
        result.reason = "below_intrinsic_value";
        return result;
    }

    double iv = 0.0;
    if (!implied_volatility(input, &iv)) {
        result.reason = "iv_not_solved";
        return result;
    }

    double d1 = 0.0;
    double d2 = 0.0;
    if (!d1_d2(input.underlying_price, input.strike_price, input.years_to_expiry,
               input.risk_free_rate, iv, &d1, &d2)) {
        result.reason = "greeks_not_solved";
        return result;
    }

    const double sqrt_t = std::sqrt(input.years_to_expiry);
    const double pdf_d1 = normal_pdf(d1);
    const double discount = std::exp(-input.risk_free_rate * input.years_to_expiry);
    result.implied_volatility = iv;

    // Deep OTM/ITM: when |d1| > 8 the pdf is ~0 and Greeks are negligible
    // 深度虚值/实值：|d1| > 8 时 PDF 近似为 0，希腊字母可忽略不计
    if (std::abs(d1) > 8.0) {
        result.gamma = 0.0;
        result.vega = 0.0;
        result.theta = 0.0;
        result.delta = (d1 > 0.0) ? (input.type == OptionType::Call ? 1.0 : 0.0)
                                   : (input.type == OptionType::Call ? 0.0 : -1.0);
    } else {
        result.gamma = pdf_d1 / (input.underlying_price * iv * sqrt_t);
        result.vega = input.underlying_price * pdf_d1 * sqrt_t / 100.0;
        if (input.type == OptionType::Call) {
            result.delta = normal_cdf(d1);
            result.theta = (-(input.underlying_price * pdf_d1 * iv) / (2.0 * sqrt_t) -
                            input.risk_free_rate * input.strike_price * discount * normal_cdf(d2)) / 365.0;
        } else {
            result.delta = normal_cdf(d1) - 1.0;
            result.theta = (-(input.underlying_price * pdf_d1 * iv) / (2.0 * sqrt_t) +
                            input.risk_free_rate * input.strike_price * discount * normal_cdf(-d2)) / 365.0;
        }
    }

    result.rho = input.strike_price * input.years_to_expiry * discount *
                 (input.type == OptionType::Call ? normal_cdf(d2) : -normal_cdf(-d2)) / 100.0;
    result.time_value = (std::max)(0.0, result.time_value);
    result.theoretical_price = black_scholes_price(input.type, input.underlying_price,
                                                   input.strike_price, input.years_to_expiry,
                                                   input.risk_free_rate, iv);
    result.ok = std::isfinite(result.implied_volatility) &&
                std::isfinite(result.delta) &&
                std::isfinite(result.gamma) &&
                std::isfinite(result.theta) &&
                std::isfinite(result.vega) &&
                std::isfinite(result.rho) &&
                std::isfinite(result.theoretical_price);
    if (!result.ok) {
        result.reason = "non_finite_result";
    }
    return result;
}

} // namespace hft
