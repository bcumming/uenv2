// vim: ts=4 sts=4 sw=4 et
#include <unistd.h>

#include <string>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <spdlog/spdlog.h>

#include <uenv/env.h>
#include <util/expected.h>
#include <util/shell.h>

#include "help.h"
#include "start.h"
#include "terminal.h"
#include "uenv.h"
#include "util.h"

namespace uenv {

std::string start_footer();

void start_args::add_cli(CLI::App& cli,
                         [[maybe_unused]] global_settings& settings) {
    auto* start_cli = cli.add_subcommand("start", "start a uenv session");
    start_cli->add_option("-v,--view", view_description,
                          "comma separated list of views to load");
    start_cli
        ->add_option("uenv", uenv_description,
                     "comma separated list of uenv to mount")
        ->required();
    start_cli->add_flag("--ignore-tty", ignore_tty,
                        "don't check for non-interactive shells");
    start_cli->add_flag(
        "-V,--no-default-view", disable_default_view,
        "disable loading default views when no view is specified");
    start_cli->callback(
        [&settings]() { settings.mode = uenv::cli_mode::start; });
    start_cli->footer(start_footer);
}

// check whether running in a tty session.
// uenv start doesn't make sense when not tty - it is designed for running
// an interactive session.
std::optional<std::string>
detect_non_interactive(const envvars::state& envvars) {
    if (!isatty(fileno(stdout))) {
        return "stdout is redirected";
    }
    if (!isatty(fileno(stdin))) {
        return "stdin is redirected";
    }
    if (envvars.get("BASH_EXECUTION_STRING")) {
        return "BASH_EXECUTION_STRING is set";
    }
    // don't check PS1 - PS1 is not exported by tools like oh-my-posh in zsh
    return std::nullopt;
}

int start(const start_args& args, const global_settings& settings) {
    spdlog::info("start with options {}", args);

    // error if a uenv is already mounted
    if (in_uenv_session(settings.calling_environment)) {
        term::error("{}",
                    R"(a uenv session is already running.
It is not possible to call 'uenv start' or 'uenv run' inside a uenv session.
You need to finish the current session by typing 'exit' or hitting '<ctrl-d>'.)");
        return 1;
    }

    if (auto reason = detect_non_interactive(settings.calling_environment);
        !args.ignore_tty && reason) {
        term::error(
            "{}", fmt::format(
                      R"('uenv start' must be run in an interactive shell ({}).

Use the flag --ignore-tty to skip this check.

This error will occur if uenv start is called within contexts like the following:
 passing
- inside .bashrc
- in a slurm batch script
- in a bash script

If your intention is to execute a command in a uenv, use run.
See '{}' for more information

If your intention is to initialize an environment (like module load), uenv start
will not work, because it starts a new interactive shell.)",
                      reason.value(), "uenv run --help"));
        return 1;
    }

    const auto resolved =
        resolve_uenv_args(args.uenv_description, settings.config.repos,
                          settings.config.system_name);
    if (!resolved) {
        term::error("{}", resolved.error());
        term::hint("{}",
                   (settings.config.repos.empty() ? messages::no_repos()
                                                  : messages::no_matches()));
        return 1;
    }

    const auto env = concretise_env(
        resolved.value(), args.view_description, args.uenv_description,
        settings.config.repos, !args.disable_default_view);

    if (!env) {
        term::error("{}", env.error());
        return 1;
    }

    auto runtime_environment = generate_environment(
        *env, settings.calling_environment, "SQFSMNT_FWD_");

    // find the current shell (zsh, bash, etc)
    auto shell = util::current_shell(settings.calling_environment);
    if (!shell) {
        term::error("unable to determine the current shell because {}",
                    shell.error());
        return 1;
    }
    spdlog::info("using shell: {}", shell->string());

    std::vector<std::string> mounts;
    for (auto m : env->uenvs) {
        mounts.push_back(fmt::format("{}:{}", m.second.sqfs_path.string(),
                                     m.second.mount_path));
    }

    const auto commands = uenv::squashfs_mount_args(
        settings.calling_environment, mounts, false /* join */,
        settings.verbose, {shell->string()});

    auto c_env = runtime_environment.c_env();
    auto error = util::exec(commands, c_env);

    // it is always an error if this code is executed, because that implies that
    // execvp failed.
    envvars::c_env_free(c_env);
    term::error("{}", error.message);
    return error.rcode;
}

std::string start_footer() {
    using enum help::block::admonition;
    using help::block;
    using help::linebreak;
    using help::lst;
    std::vector<help::item> items{
        // clang-format off
        block{none, "Start a new shell with a uenv environment. The shell will be",
                          fmt::format("the default shell set using the SHELL envronment variable ({}).", lst("echo $SHELL"))},
        linebreak{},
        block{note,
                "the uenv must have been pulled before it can be used. See the list",
                fmt::format("of available uenv using {}.", lst("uenv image ls")),
                "If using a path to a squashfs file, you need to have read rights in",
                "the path where the file is stored.",
        },
        linebreak{},
        block{xmpl, "start a uenv"},
        block{code,   "uenv start prgenv-gnu/24.7:v3"},
        block{none, "use the full name/version:tag format to disambiguate fully the image "},
        linebreak{},
        block{info, "uenv will mount the image at the correct location, which for most uenv",
                          "is /user-enviroment."},
        linebreak{},
        block{xmpl, fmt::format("start an image built for the system daint using {}", lst("@daint"))},
        block{code,   "uenv start prgenv-gnu/24.7:v1@daint"},
        linebreak{},
        block{xmpl, "use the @ symbol to specify a target system name"},
        block{code,   "uenv start prgenv-gnu@todi"},
        block{none, "this feature is useful when using images that were built for a different system",
                          "than the one you are currently working on."},
        linebreak{},
        block{xmpl, "two uenv images can be used at the same time"},
        block{code,   "uenv start prgenv-gnu/24.7:v3,editors/24.7:v1"},
        linebreak{},
        block{note, "to start two uenv at the same time, they must be mounted at different mount.",
                          "points. uenv provided by CSCS are designed to be mounted at two locations:",
                          fmt::format("  - {} programming environments and applications", help::lst("/user-enviroment")),
                          fmt::format("  - {} tools like debuggers and profilers that are used", help::lst("/user-tools")),
                                      "    alongside PE and applications"},
        linebreak{},
        block{xmpl, "example of using the full specification:"},
        block{code,   "uenv start prgenv-gnu/24.7:v3:/user-environemnt,editors/24.7:v1:/user-tools \\",
                      "           --view=prgenv-gnu:default,editors:modules"},
        linebreak{},
        block{xmpl, "run a uenv using a squashfs file:"},
        block{code,   "uenv start --view=develop ./store.squashfs"},
        block{code,   "uenv start $SCRATCH/images/gromacs/store.squashfs"},
        linebreak{},
        block{note, "The path of a squashfs file must be either a relative path"},
        block{none, "(starting with ./ or ../), or an absolute path."},
        // clang-format on
    };

    return fmt::format("{}", fmt::join(items, "\n"));
}
} // namespace uenv
