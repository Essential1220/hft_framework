#pragma once

#include "common/config.h"

#include <functional>
#include <string>
#include <vector>

namespace hft {

struct ConfigValidationError {
    std::string section;
    std::string key;
    std::string message;
};

class ConfigValidator {
public:
    bool validate(const Config& config, std::vector<ConfigValidationError>& errors) const;

private:
    void check_accounts(const Config& config, std::vector<ConfigValidationError>& errors) const;
    void check_risk(const Config& config, std::vector<ConfigValidationError>& errors) const;
    void check_strategies(const Config& config, std::vector<ConfigValidationError>& errors) const;
    void check_log(const Config& config, std::vector<ConfigValidationError>& errors) const;
    void check_web(const Config& config, std::vector<ConfigValidationError>& errors) const;
    void check_performance(const Config& config, std::vector<ConfigValidationError>& errors) const;
    void check_runtime(const Config& config, std::vector<ConfigValidationError>& errors) const;
};

} // namespace hft
