#include "util/expected.h"
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <spdlog/spdlog.h>

#include <uenv/config.h>
#include <uenv/log.h>
#include <uenv/mount.h>
#include <uenv/mount_kernel.h>
#include <uenv/parse.h>
#include <util/color.h>
#include <util/envvars.h>
#include <util/shell.h>

// print a formtted error message and exit with return code 1
template <typename... T>
void error_and_exit(fmt::format_string<T...> fmt, T&&... args) {
    fmt::print(stderr, "{}: {}\n", ::color::red("error"),
               fmt::vformat(fmt, fmt::make_format_args(args...)));
    exit(1);
}

// squashfs-mount --sqfs=file:mount[,file:mount] -- cmd [args]
//
// --version --verbose=2, -v, -vv, -vvv
int main(int argc, char** argv, char** envp) {
    //
    // Capture the environment variables
    //

    const auto calling_env = envvars::state(envp);

    // get the uid before performing any privilege/namespace changes
    const uid_t uid = getuid();

    //
    // Command line argument parsing
    //

    bool print_version = false;
    int verbosity = 1;
    std::optional<std::string> raw_mounts;
    std::optional<std::vector<std::string>> commands;

    CLI::App cli(fmt::format("squashfs-mount {}", UENV_VERSION));
    cli.add_flag("-v,--verbose", verbosity, "enable verbose output");
    cli.add_flag("--version", print_version, "print version");
    cli.add_option("-s,--sqfs", raw_mounts,
                   "comma separated list of squashfs files to mount");
    cli.add_option("commands", commands,
                   "the command to run, including with arguments");

    CLI11_PARSE(cli, argc, argv);

    //
    // print version and quit if --version flag was used
    //

    if (print_version) {
        fmt::println("{}", UENV_VERSION);
        return 0;
    }

    //
    // check that required arguments have been set.
    //
    if (!commands) {
        error_and_exit("no command given");
    }

    //
    // set logging level
    //

    // By default there is no logging to the console
    // The level of logging is increased by adding --verbose
    spdlog::level::level_enum console_log_level = spdlog::level::off;
    if (verbosity == 1) {
        console_log_level = spdlog::level::info;
    } else if (verbosity == 2) {
        console_log_level = spdlog::level::debug;
    } else if (verbosity >= 3) {
        console_log_level = spdlog::level::trace;
    }
    // note: syslog uses level::info to capture key events
    uenv::init_log(console_log_level);

    //
    // validate the mount points
    //

    uenv::mount_list mounts;
    if (raw_mounts) {
        auto r = uenv::parse_and_validate_mounts(*raw_mounts);
        if (!r) {
            error_and_exit("{}", r.error());
        }
        mounts = r.value();
    }
    const std::string uenv_mount_list =
        fmt::format("{}", fmt::join(mounts, ","));

    spdlog::info("uenv_mount_list {}", uenv_mount_list);
    spdlog::info("commands ['{}']", fmt::join(*commands, "', '"));

    //
    // mount the squashfs images with the kernel squashfs driver:
    //  * unshare the mount namespace and become the real root user, so that
    //    the images can be mounted;
    //  * mount each image with libmount;
    //  * drop back to the calling user and disallow gaining new privileges.
    //
    if (!mounts.empty()) {
        if (auto r = uenv::unshare_and_become_root(); !r) {
            error_and_exit("{}", r.error());
        }
        if (auto r = uenv::do_mount(mounts); !r) {
            error_and_exit("mount failed {}", r.error());
        }
    } else {
        spdlog::warn("nothing mounted (no --sqfs flag provided)");
    }

    // the setuid binary always starts with an elevated effective uid, so it
    // has to be dropped even when there is nothing to mount.
    if (auto r = uenv::return_to_user_and_no_new_privs(uid); !r) {
        error_and_exit("{}", r.error());
    }

    //
    // Generate the runtime environment variables
    //

    envvars::state runtime_env{};

    // forward all environment variables not prefixed with SQFSMNT_FWD_
    for (auto& [name, v] : calling_env.variables()) {
        if (!name.starts_with("SQFSMNT_FWD_")) {
            // use forward instead of set, becase set drops environment
            // variables that do not have valid POSIX compliant names.
            // In this context we need to tolerate all possible names to fully
            // reproduce the calling environment.
            // For example: bash function exports do not follow the POSIX
            // standard.

            runtime_env.forward(name, v);
        }
    }
    // add the forwarded variables in a second loop, in case a variable with the
    // same name was already in the calling environment.
    for (auto& [name, v] : calling_env.variables()) {
        if (name.starts_with("SQFSMNT_FWD_")) {
            // use set, which will still drop environment variables with invalid
            // names, because an invalid name in this context is certainly a bug
            // in the caller.
            runtime_env.set(name.substr(12), v);
        }
    }

    runtime_env.set("UENV_MOUNT_LIST", uenv_mount_list);

    auto cenv = runtime_env.c_env();
    auto error = util::exec(*commands, cenv);

    // it is always an error if this code is executed, because that implies that
    // execvp failed.
    envvars::c_env_free(cenv);
    error_and_exit("{}", error.message);

    return error.rcode;
}
