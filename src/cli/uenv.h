// vim: ts=4 sts=4 sw=4 et
#pragma once

#include <fmt/core.h>

namespace uenv {

extern int mode;
// extern bool verbose;

constexpr int mode_none = 0;
constexpr int mode_start = 1;
// constexpr int mode_status = 1;
// constexpr int mode_image = 2;
// constexpr int mode_run = 3;

struct global_settings {
    int verbose = false;
    bool no_color = false;
    int mode = mode_none;

    // bool color = true;
    // std::string repo;
    // std::string const to_string()
};

} // namespace uenv

template <> class fmt::formatter<uenv::global_settings> {
  public:
    // parse format specification and store it:
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.end();
    }
    // format a value using stored specification:
    template <typename FmtContext>
    constexpr auto format(uenv::global_settings const& opts,
                          FmtContext& ctx) const {
        return fmt::format_to(ctx.out(), "global_settings(mode {}, verbose {})",
                              opts.mode, opts.verbose);
    }
};
