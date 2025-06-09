// vim: ts=4 sts=4 sw=4 et

#include <filesystem>
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

#include "copy.h"
#include "help.h"
#include "terminal.h"

namespace uenv {

std::string image_copy_footer();

void image_copy_args::add_cli(CLI::App& cli,
                              [[maybe_unused]] global_settings& settings) {
    auto* copy_cli =
        cli.add_subcommand("copy", "copy a uenv inside a remote registry");
    copy_cli
        ->add_option("source-uenv", src_uenv_description,
                     "either name/version:tag, sha256 or id")
        ->required();
    copy_cli->add_option("dest-uenv", dst_uenv_description, "label to copy to")
        ->required();
    copy_cli->add_option(
        "--token", token,
        "a path that contains a TOKEN file for accessing restricted uenv");
    copy_cli->add_option("--username", username,
                         "user name for accessing restricted uenv.");
    copy_cli->add_flag("--force", force,
                       "overwrite the destination if it exists");
    copy_cli->callback(
        [&settings]() { settings.mode = uenv::cli_mode::image_copy; });

    copy_cli->footer(image_copy_footer);
}

int image_copy([[maybe_unused]] const image_copy_args& args,
               [[maybe_unused]] const global_settings& settings) {
    std::optional<uenv::oras::credentials> credentials;
    if (auto c = site::get_credentials(args.username, args.token)) {
        credentials = *c;
    } else {
        term::error("{}", c.error());
        return 1;
    }

    uenv_nslabel src_label{};
    if (const auto parse = parse_uenv_nslabel(args.src_uenv_description)) {
        src_label = *parse;
    } else {
        term::error("invalid source: {}", parse.error().message());
        return 1;
    }
    if (!src_label.nspace || !src_label.label.name) {
        term::error("the source uenv {} must provide at least a namespace and "
                    "name, e.g. 'build::f7076704830c8de7'",
                    args.src_uenv_description);
        return 1;
    }
    spdlog::debug("source label {}::{}", src_label.nspace, src_label.label);

    uenv_nslabel dst_label{};
    if (const auto parse = parse_uenv_nslabel(args.dst_uenv_description)) {
        dst_label = *parse;
    } else {
        term::error("invalid destination: {}", parse.error().message());
        return 1;
    }
    spdlog::debug("destination label {}::{}", dst_label.nspace,
                  dst_label.label);
    if (!dst_label.nspace) {
        term::error("the destination uenv {} must provide at least a "
                    "namespace and name, e.g. 'deploy::'",
                    args.dst_uenv_description);
        return 1;
    }

    auto src_registry = site::registry_listing(*src_label.nspace);
    if (!src_registry) {
        term::error("unable to get a listing of the uenv",
                    src_registry.error());
        return 1;
    }

    // search db for matching records
    const auto src_matches = src_registry->query(src_label.label);
    if (!src_matches) {
        term::error("invalid search term: {}", src_registry.error());
        return 1;
    }
    // check that there is one record with a unique sha
    if (src_matches->empty()) {
        using enum help::block::admonition;
        term::error("no uenv found that matches '{}'\n\n{}",
                    args.src_uenv_description,
                    help::block(info, "try searching for the uenv to copy "
                                      "first using 'uenv image find'"));
        return 1;
    } else if (!src_matches->unique_sha()) {
        std::string errmsg =
            fmt::format("more than one uenv found that matches '{}':\n",
                        args.src_uenv_description);
        errmsg += format_record_set(*src_matches);
        term::error("{}", errmsg);
        return 1;
    }

    // pick a record to use for pulling
    const auto src_record = *(src_matches->begin());
    spdlog::info("source record: {} {}", src_record.sha, src_record);

    // create the destination record
    auto dst_record = src_record;
    {
        const auto& dl = dst_label.label;
        if (dl.name) {
            dst_record.name = *(dl.name);
        }
        if (dl.tag) {
            dst_record.tag = *(dl.tag);
        }
        if (dl.version) {
            dst_record.version = *(dl.version);
        }
        if (dl.system) {
            dst_record.system = *(dl.system);
        }
        if (dl.uarch) {
            dst_record.uarch = *(dl.uarch);
        }
    }

    if (dst_record == src_record) {
        term::error("the source and destination are the same");
        return 1;
    }

    spdlog::info("destination record: {} {}", dst_record.sha, dst_record);

    // check whether the destination already exists
    auto dst_registry = site::registry_listing(*dst_label.nspace);
    if (dst_registry && dst_registry->contains(dst_record)) {
        if (!args.force) {
            term::error("the destination already exists - use the --force flag "
                        "to copy anyway");
            return 1;
        }
        term::error("the destination already exists and will be overwritten");
    }

    if (!settings.config.registry) {
        term::error("registry is not configured - set it in the config file or provide --registry option");
        return 1;
    }
    const auto rego_url = settings.config.registry.value();
    spdlog::debug("registry url: {}", rego_url);
    if (auto result =
            oras::copy(rego_url, src_label.nspace.value(), src_record,
                       dst_label.nspace.value(), dst_record, credentials);
        !result) {
        term::error("unable to copy uenv.\n{}", result.error().message);
        return 1;
    }

    term::msg("copied {}::{}", src_label.nspace.value(), src_record);
    term::msg("to     {}::{}", dst_label.nspace.value(), dst_record);

    return 0;
}

std::string image_copy_footer() {
    using enum help::block::admonition;
    std::vector<help::item> items{
        // clang-format off
        help::block{none, "Copy a uenv to a new location inside a remote registry." },
        help::linebreak{},
        help::linebreak{},
        help::block{xmpl, "deploy a uenv from build to deploy namespace"},
        help::block{code,   "uenv image copy prgenv-gnu/24.11:1551223269@todi%gh200 deploy:::v1"},
        help::block{code,   "uenv image copy 7890d67458ce7deb deploy:::v1"},
        help::block{none, "when deploying a build, provide a tag."},
        help::linebreak{},
        help::block{xmpl, "redeploy a uenv to a new vcluster"},
        help::block{code,   "uenv image copy 7890d67458ce7deb deploy::@daint"},
        help::block{code,   "uenv image copy 7890d67458ce7deb @daint"},
        help::block{none, "in this case, the uenv will deployed with the current tag."},
        // clang-format on
    };

    return fmt::format("{}", fmt::join(items, "\n"));
}

} // namespace uenv
