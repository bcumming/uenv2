// vim: ts=4 sts=4 sw=4 et

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

#include "delete.h"
#include "help.h"
#include "terminal.h"

namespace uenv {

std::string image_delete_footer();

void image_delete_args::add_cli(CLI::App& cli,
                                [[maybe_unused]] global_settings& settings) {
    auto* delete_cli =
        cli.add_subcommand("delete", "delete a uenv from a remote registry");
    delete_cli
        ->add_option("uenv", uenv_description,
                     "either name/version:tag, sha256 or id")
        ->required();
    delete_cli
        ->add_option(
            "--token", token,
            "a path that contains a TOKEN file for accessing restricted uenv")
        ->required();
    delete_cli->add_option("--username", username,
                           "user name for accessing restricted uenv.");
    delete_cli->callback(
        [&settings]() { settings.mode = uenv::cli_mode::image_delete; });

    delete_cli->footer(image_delete_footer);
}

int image_delete([[maybe_unused]] const image_delete_args& args,
                 [[maybe_unused]] const global_settings& settings) {
    uenv::oras::credentials credentials;
    if (auto c = site::get_credentials(args.username, args.token)) {
        if (!*c) {
            term::error("full credentials must be provided", c.error());
        }
        credentials = (*c).value();
    } else {
        term::error("{}", c.error());
        return 1;
    }
    spdlog::debug("registry credentials: {}", credentials);

    uenv_label label{};
    std::string nspace{};
    if (const auto parse = parse_uenv_nslabel(args.uenv_description)) {
        label = parse->label;
        if (!label.name || !parse->nspace) {
            term::error("the uenv {} must provide at least a namespace and "
                        "name, e.g. 'build::f7076704830c8de7'",
                        args.uenv_description);
            return 1;
        }
        nspace = parse->nspace.value();
    } else {
        term::error("invalid uenv: {}", parse.error().message());
        return 1;
    }
    spdlog::debug("requested to delete {}::{}", nspace, label);

    auto registry = site::registry_listing(nspace);
    if (!registry) {
        term::error("unable to get a listing of the uenv", registry.error());
        return 1;
    }

    // search db for matching records
    const auto matches = registry->query(label);
    if (!matches) {
        term::error("invalid search term: {}", registry.error());
        return 1;
    }
    // check that there is one record with a unique sha
    if (matches->empty()) {
        using enum help::block::admonition;
        term::error("no uenv found that matches '{}'\n\n{}",
                    args.uenv_description,
                    help::block(info, "try searching for the uenv to copy "
                                      "first using 'uenv image find'"));
        return 1;
    } else if (!matches->unique_sha()) {
        std::string errmsg =
            fmt::format("more than one sha found that matches '{}':\n",
                        args.uenv_description);
        errmsg += format_record_set(*matches);
        term::error("{}", errmsg);
        return 1;
    }

    if (!settings.config.registry) {
        term::error("registry is not configured - set it in the config file or provide --registry option");
        return 1;
    }
    const auto rego_url = settings.config.registry.value();
    spdlog::debug("registry url: {}", rego_url);
    for (auto& record : *matches) {
        auto url = fmt::format(
            "https://jfrog.svc.cscs.ch/artifactory/uenv/{}/{}/{}/{}/{}/{}",
            nspace, record.system, record.uarch, record.name, record.version,
            record.tag);

        if (auto result =
                util::curl::del(url, credentials.username, credentials.token);
            !result) {
            term::error("unable to delete uenv: {}", result.error().message);
            return 1;
        }

        term::msg("delete {}", url);
    }

    return 0;
}

std::string image_delete_footer() {
    using enum help::block::admonition;
    std::vector<help::item> items{
        // clang-format off
        help::block{none, "Delete a uenv from a remote registry." },
        help::linebreak{},
        help::linebreak{},
        help::block{xmpl, "deploy a uenv from build to deploy namespace"},
        help::block{code,   "uenv image delete build::prgenv-gnu/24.11:1551223269@todi%gh200"},
        help::block{code,   "uenv image delete build::7890d67458ce7deb"},
        help::block{code,   "uenv image delete deploy::prgenv-gnu/24.11:rc1@todi"},
        help::linebreak{},
        help::block{note, "the requested uenv must resolve to a unique sha."},
        // clang-format on
    };

    return fmt::format("{}", fmt::join(items, "\n"));
}

} // namespace uenv
