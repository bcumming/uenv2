// vim: ts=4 sts=4 sw=4 et

#include <string>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <spdlog/spdlog.h>

#include <uenv/config.h>
#include <uenv/env.h>
#include <util/expected.h>
#include <util/shell.h>
#include <util/subprocess.h>

#include "help.h"
#include "run.h"
#include "terminal.h"
#include "uenv.h"
#include "util.h"

namespace uenv {

std::string run_footer();

void run_args::add_cli(CLI::App& cli, global_settings& settings) {
    auto* run_cli = cli.add_subcommand("run", "run a uenv session");
    run_cli->add_option("-v,--view", view_description,
                        "comma separated list of views to load");
    run_cli
        ->add_option("uenv", uenv_description,
                     "comma separated list of uenv to mount")
        ->required();
    run_cli
        ->add_option("commands", commands,
                     "the command to run, including with arguments")
        ->required();

    run_cli->add_flag(
        "-V,--no-default-view", disable_default_view,
        "disable loading default views when no view is specified");

    // the --join flag is only meaningful for the FUSE backend, where a
    // single task mounts and the others join its namespaces. The
    // setuid/kernel backend mounts independently in every task.
    if constexpr (uenv::backend_fuse) {
        run_cli->add_flag("-j,--join", join,
                          "join namespaces of tasks on the same node");
    }

    run_cli->callback([&settings]() { settings.mode = uenv::cli_mode::run; });
    run_cli->footer(run_footer);
}

int run(const run_args& args, const global_settings& settings) {
    spdlog::info("run with options {}", args);

    // error if a uenv is already mounted
    if (in_uenv_session(settings.calling_environment)) {
        term::error("{}",
                    R"(a uenv session is already running.
It is not possible to call 'uenv start' or 'uenv run' inside a uenv session.
You need to finish the current session by typing 'exit' or hitting '<ctrl-d>'.)");
        return 1;
    }

    if (settings.config.repos.empty()) {
        term::warn("{}", messages::no_repos());
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

    std::vector<std::string> mounts;
    for (auto m : env->uenvs) {
        mounts.push_back(fmt::format("{}:{}", m.second.sqfs_path.string(),
                                     m.second.mount_path));
    }

    const auto commands =
        uenv::squashfs_mount_args(settings.calling_environment, mounts,
                                  args.join, settings.verbose, args.commands);

    auto c_env = runtime_environment.c_env();
    auto error = util::exec(commands, c_env);

    // it is always an error if this code is executed, because that implies
    // that execvp failed.
    envvars::c_env_free(c_env);
    term::error("{}", error.message);
    return error.rcode;
}

std::string run_footer() {
    using enum help::block::admonition;
    using help::block;
    using help::linebreak;
    using help::lst;
    std::vector<help::item> items{
        // clang-format off
        block{none, "Run a command in an environment."},
        linebreak{},
        block{xmpl, "run the script job.sh in an evironmnent"},
        block{code,   "uenv run prgenv-gnu/24.2:v1 -- ./job.sh"},
        block{none, "This will mount prgenv-gnu, execute job.sh, then return to the calling shell."},
        linebreak{},
        block{note, "how the command to execute comes after the two dashes '--'."},
        linebreak{},
        block{xmpl, "run the script job.sh in an evironmnent with a view loaded"},
        block{code,   "uenv run prgenv-gnu/24.2:v1 --view=default -- ./job.sh"},
        linebreak{},
        block{note, "the spec must uniquely identify the uenv. To ensure this, always use a"},
        block{none, "fully qualified spec in the form of name/version:tag, the unique 16 digit id,"},
        block{none, "or sha256 of a uenv. If more than one uenv match the spec, an error message"},
        block{none, "is printed."},
        linebreak{},
        block{xmpl, "run the job.sh script with two images mounted"},
        block{code,   "uenv run prgenv-gnu/24.2:v1,ddt/23.1 -- ./job.sh"},
        linebreak{},
        block{xmpl, "run the job.sh script with two images mounted at specific mount points"},
        block{code,   "uenv run prgenv-gnu/24.2:v1:$SCRATCH/pe,ddt/23.1:/user-tools -- ./job.sh"},
        block{none, "Here the mount point for each image is specified using a ':'."},
        linebreak{},
        block{note, "uenv must be mounted at the mount point for which they were built."},
        block{none, "If mounted at the wrong location, a warning message will be printed, and"},
        block{none, "views will be disabled."},
        linebreak{},
        block{xmpl, "the run command can be used to execute workflow steps with", "separate environments"},
        block{code,   "uenv run gromacs/23.1  -- ./simulation.sh"},
        block{code,   "uenv run paraview/5.11 -- ./render.sh"},
        linebreak{},
        block{xmpl, "run a uenv using a squashfs file:"},
        block{code,   "uenv run --view=tools ./store.squashfs -- nvim main.c"},
        block{code,   "uenv run $SCRATCH/images/gromacs/store.squashfs -- ./simulation.sh"},
        linebreak{},
        block{note, "The path of a squashfs file must be either a relative path"},
        block{none, "(starting with ./ or ../), or an absolute path."},
        // clang-format on
    };

    return fmt::format("{}", fmt::join(items, "\n"));
}

} // namespace uenv
