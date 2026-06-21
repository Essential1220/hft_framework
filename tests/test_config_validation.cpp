#include "common/config.h"
#include "common/config_validator.h"

#include <stdexcept>
#include <string>
#include <vector>

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

static hft::Config make_valid_config() {
    hft::Config cfg;
    cfg.set_string("Accounts", "List", "210037");
    cfg.set_string("Accounts", "MarketDataAccount", "210037");
    cfg.set_string("CTP.210037", "BrokerID", "9999");
    cfg.set_string("CTP.210037", "UserID", "210037");
    cfg.set_string("CTP.210037", "Password", "ENC:xxx");
    cfg.set_string("CTP.210037", "TradeFront", "tcp://1.2.3.4:10201");
    cfg.set_string("CTP.210037", "MarketFront", "tcp://1.2.3.4:10211");
    cfg.set_string("Risk", "MaxOrderSize", "5");
    cfg.set_string("Risk", "MaxNetPosition", "10");
    cfg.set_string("Risk", "MaxOrdersPerMinute", "30");
    cfg.set_string("Risk", "MaxCancelRate", "0.5");
    cfg.set_string("Risk", "MaxDailyLoss", "5000");
    cfg.set_string("Risk", "CancelRateWindowMinutes", "60");
    cfg.set_string("Log", "Level", "INFO");
    cfg.set_string("Web", "Port", "9090");
    cfg.set_string("Runtime", "RunMode", "service");
    return cfg;
}

void test_config_valid_passes() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    TEST_ASSERT(validator.validate(cfg, errors));
    TEST_ASSERT_EQ(errors.size(), 0u);
}

void test_config_missing_account_section() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    hft::Config cfg;
    cfg.set_string("Accounts", "List", "999999");
    cfg.set_string("Accounts", "MarketDataAccount", "999999");
    cfg.set_string("Log", "Level", "INFO");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.section == "CTP.999999") found = true;
    }
    TEST_ASSERT(found);
}

void test_config_md_account_not_in_list() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    cfg.set_string("Accounts", "MarketDataAccount", "nonexistent");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.key == "MarketDataAccount" && e.message.find("not in Accounts.List") != std::string::npos)
            found = true;
    }
    TEST_ASSERT(found);
}

void test_config_risk_invalid_cancel_rate() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    cfg.set_string("Risk", "MaxCancelRate", "1.5");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.key == "MaxCancelRate") found = true;
    }
    TEST_ASSERT(found);
}

void test_config_risk_negative_order_size() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    cfg.set_string("Risk", "MaxOrderSize", "-1");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.key == "MaxOrderSize") found = true;
    }
    TEST_ASSERT(found);
}

void test_config_invalid_log_level() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    cfg.set_string("Log", "Level", "VERBOSE");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.key == "Level" && e.section == "Log") found = true;
    }
    TEST_ASSERT(found);
}

void test_config_invalid_web_port() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    cfg.set_string("Web", "Port", "99999");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.key == "Port") found = true;
    }
    TEST_ASSERT(found);
}

void test_config_strategy_missing_section() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    cfg.set_string("Strategies", "List", "ghost_strategy");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.message.find("ghost_strategy") != std::string::npos) found = true;
    }
    TEST_ASSERT(found);
}

void test_config_strategy_python_no_script() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    cfg.set_string("Strategies", "List", "s1");
    cfg.set_string("Strategy.s1", "Type", "python");
    cfg.set_string("Strategy.s1", "Instruments", "rb2610");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.key == "ScriptPath") found = true;
    }
    TEST_ASSERT(found);
}

void test_config_invalid_run_mode() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    auto cfg = make_valid_config();
    cfg.set_string("Runtime", "RunMode", "daemon");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.key == "RunMode") found = true;
    }
    TEST_ASSERT(found);
}

void test_config_empty_accounts_warns() {
    hft::ConfigValidator validator;
    std::vector<hft::ConfigValidationError> errors;
    hft::Config cfg;
    cfg.set_string("Log", "Level", "INFO");
    TEST_ASSERT(!validator.validate(cfg, errors));
    bool found = false;
    for (const auto& e : errors) {
        if (e.section == "Accounts" && e.key == "List") found = true;
    }
    TEST_ASSERT(found);
}
