// vim: ts=4 sts=4 sw=4 et

#include <CLI/CLI.hpp>
#include <fmt/color.h>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <uenv/log.h>

#include "start.h"
#include "uenv.h"

void help() {
    fmt::println("usage main");
}

int main(int argc, char** argv) {
    uenv::global_settings settings;

    CLI::App cli("uenv");
    cli.add_flag("-v,--verbose", settings.verbose, "enable verbose output");
    cli.add_flag("--no-color", settings.no_color, "disable color output");

    uenv::start_args start_args;
    start_args.add_cli(cli, settings);

    CLI11_PARSE(cli, argc, argv);

    // Warnings and errors are always logged. The verbosity level is increased
    // with repeated uses of --verbose.
    spdlog::level::level_enum console_log_level = spdlog::level::warn;
    if (settings.verbose == 1) {
        console_log_level = spdlog::level::info;
    } else if (settings.verbose == 2) {
        console_log_level = spdlog::level::debug;
    } else if (settings.verbose >= 3) {
        console_log_level = spdlog::level::trace;
    }
    uenv::init_log(console_log_level, spdlog::level::trace);

    spdlog::debug("{}", settings);

    switch (settings.mode) {
    case uenv::mode_start:
        return uenv::start(start_args, settings);
    case uenv::mode_none:
    default:
        help();
    }

    return 0;
}
