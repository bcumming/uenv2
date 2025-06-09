// vim: ts=4 sts=4 sw=4 et

#include <csignal>
#include <string>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <site/site.h>
#include <uenv/oras.h>
#include <uenv/parse.h>
#include <uenv/print.h>
#include <uenv/repository.h>
#include <util/curl.h>
#include <util/expected.h>
#include <util/fs.h>
#include <util/signal.h>

#include "help.h"
#include "push.h"
#include "terminal.h"
#include "util.h"

namespace uenv {

std::string image_push_footer();

void image_push_args::add_cli(CLI::App& cli,
                              [[maybe_unused]] global_settings& settings) {
    auto* push_cli = cli.add_subcommand("push", "push a uenv to a registry");
    push_cli
        ->add_option("source", source,
                     "the local uenv to push, either name/version:tag, sha256, "
                     "id, or the path of a SquashFS file")
        ->required();
    push_cli
        ->add_option("dest", dest,
                     "the destination in the full "
                     "namespace::name/version:tag@system%uarch form")
        ->required();
    push_cli->add_option(
        "--token", token,
        "a path that contains a TOKEN file for accessing the registry");
    push_cli->add_option("--username", username,
                         "user name for the registry (by default the username "
                         "on the system will be used).");
    push_cli->add_flag("--force", force,
                       "overwrite the destination if it exists");
    push_cli->callback(
        [&settings]() { settings.mode = uenv::cli_mode::image_push; });

    push_cli->footer(image_push_footer);
}

int image_push([[maybe_unused]] const image_push_args& args,
               [[maybe_unused]] const global_settings& settings) {
    std::optional<uenv::oras::credentials> credentials;
    if (auto c = site::get_credentials(args.username, args.token)) {
        credentials = *c;
    } else {
        term::error("{}", c.error());
        return 1;
    }

    // parse and validate the destination (i.e. the label in the registry)
    uenv_nslabel dst_label{};
    if (const auto parse = parse_uenv_nslabel(args.dest)) {
        dst_label = *parse;
    } else {
        term::error("invalid destination: {}", parse.error().message());
        return 1;
    }
    if (!dst_label.nspace || !dst_label.label.fully_qualified()) {
        term::error("the destination uenv {} must be fully qualified, e.g. "
                    "'deploy::name/version:tag%system%gh200'",
                    args.dest);
        return 1;
    }
    spdlog::debug("destination label {}::{}", dst_label.nspace,
                  dst_label.label);

    const auto nspace = dst_label.nspace.value();
    auto registry = site::registry_listing(nspace);
    if (!registry) {
        term::error("unable to get a listing of the uenv", registry.error());
        return 1;
    }

    // check whether an image that matches the target label is already
    // in the registry.
    const auto remote_matches = registry->query(dst_label.label);
    if (!remote_matches) {
        term::error("invalid search term: {}", registry.error());
        return 1;
    }
    if (!remote_matches->empty() && !args.force) {
        using enum help::block::admonition;
        term::error(
            "a uenv that matches '{}' is already in the registry\n\n{}",
            args.dest,
            help::block(info,
                        "use the --force flag if you want to overwrite it "));
        return 1;
    } else if (!remote_matches->empty() && args.force) {
        spdlog::info("{} already exists and will be overwritten", args.dest);
        term::msg("the destination already exists and will be overwritten");
    }

    // validate the source squashfs file
    auto sqfs = uenv::validate_squashfs_image(args.source);
    if (!sqfs) {
        term::error("invalid squashfs file {}: {}", args.source, sqfs.error());
        return 1;
    }
    spdlog::info("image_push: squashfs {}", sqfs.value());

    try {
        if (!settings.config.registry) {
            term::error("registry is not configured - set it in the config file or provide --registry option");
            return 1;
        }
        auto rego_url = settings.config.registry.value();
        spdlog::debug("registry url: {}", rego_url);

        // Push the SquashFS image
        auto push_result = oras::push_tag(rego_url, nspace, dst_label.label,
                                          sqfs->sqfs, credentials);
        if (!push_result) {
            term::error("unable to push uenv.\n{}",
                        push_result.error().message);
            return 1;
        }

        // Check for metadata and push if available
        if (sqfs->meta) {
            spdlog::info("image_push: pushing metadata from {}",
                         sqfs->meta.value().string());

            auto meta_result =
                oras::push_meta(rego_url, nspace, dst_label.label,
                                sqfs->meta.value(), credentials);
            if (!meta_result) {
                spdlog::warn("unable to push metadata.\n{}",
                             meta_result.error().message);
                term::warn("unable to push metadata.\n{}",
                           meta_result.error().message);
                // Continue even if metadata push fails
            } else {
                spdlog::info("successfully pushed metadata");
            }
        }

        term::msg("successfully pushed {}", args.source);
        term::msg("to {}", args.dest);
    } catch (util::signal_exception& e) {
        spdlog::info("user interrupted the upload with ctrl-c");

        // reraise the signal
        raise(e.signal);
    }

    return 0;
} // namespace uenv

std::string image_push_footer() {
    using enum help::block::admonition;
    std::vector<help::item> items{
        // clang-format off
        help::block{none, "Push a uenv to a registry." },
        help::linebreak{},
        help::linebreak{},
        help::block{xmpl, "push a uenv from your local repository"},
        help::block{code,   "uenv image push prgenv-gnu/24.11:v3 prgenv-gnu/24.11:v3%gh200@daint"},
        help::linebreak{},
        help::block{xmpl, "push a uenv from a SquashFS file on the local filesystem"},
        help::block{code,   "uenv image push ./store.squashfs prgenv-gnu/24.11:v3%gh200@daint"},
        help::linebreak{},
        help::block{xmpl, "overwrite an existing uenv in the registry"},
        help::block{code,   "uenv image push --force ./store.squashfs prgenv-gnu/24.11:v3%gh200@daint"},
        help::linebreak{},
        help::block{xmpl, "use a token for the registry"},
        help::block{code,   "uenv image push --token=/opt/cscs/uenv/tokens/vasp6 \\"},
        help::block{code,   "                ./store.squashfs prgenv-gnu/24.11:v3%gh200@daint"},
        help::block{note, "this is required if you have not configured ~/.docker/config.json" },
        help::block{none, "with the token for the registry." },
        // clang-format on
    };

    return fmt::format("{}", fmt::join(items, "\n"));
}

} // namespace uenv
