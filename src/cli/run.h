// vim: ts=4 sts=4 sw=4 et

#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "uenv.h"

namespace uenv {

void run_help();

struct run_args {
    std::string uenv_description;
    std::optional<std::string> view_description;
    bool join = false;
    std::vector<std::string> commands;
    bool disable_default_view = false;
    void add_cli(CLI::App&, global_settings& settings);
};

int run(const run_args& args, const global_settings& settings);

} // namespace uenv

#include <fmt/core.h>
#include <fmt/ranges.h>

template <> class fmt::formatter<uenv::run_args> {
  public:
    // parse format specification and store it:
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.end();
    }
    // format a value using stored specification:
    template <typename FmtContext>
    constexpr auto format(uenv::run_args const& opts, FmtContext& ctx) const {
        auto tmp = fmt::format_to(
            ctx.out(), "{{uenv: '{}', view: ", opts.uenv_description);
        if (!opts.view_description) {
            return fmt::format_to(tmp, "'', commands:  {}}}",
                                  opts.uenv_description,
                                  fmt::join(opts.commands, " "));
        }
        return fmt::format_to(tmp, "'{}', commands: {}}}",
                              *opts.view_description,
                              fmt::join(opts.commands, " "));
    }
};
