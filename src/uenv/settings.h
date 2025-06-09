#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <util/envvars.h>
#include <util/expected.h>

namespace uenv {

struct config_base {
    std::optional<std::string> repo;
    std::optional<std::string> registry;
    std::optional<bool> color;
};

// the result of parsing a line in a configuration file
struct config_line {
    std::string key;
    std::string value;
    // evaluates to false -> an empty or comment line
    operator bool() const {
        return !key.empty();
    }
};

// read configuration from the user configuration file
// the location of the config file is determined using XDG_CONFIG_HOME or HOME
util::expected<config_base, std::string>
load_user_config(const envvars::state&);

// get the default configuration
config_base default_config(const envvars::state& calling_env);

config_base merge(const config_base& lhs, const config_base& rhs);

struct configuration {
    std::optional<std::filesystem::path> repo;
    std::optional<std::filesystem::path> registry;
    bool color;
    configuration& operator=(const configuration&) = default;
};

util::expected<configuration, std::string>
generate_configuration(const config_base& base);

} // namespace uenv
