#include <algorithm>
#include <filesystem>

#include <unistd.h>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <spdlog/spdlog.h>

#include <util/color.h>
#include <util/expected.h>
#include <util/fs.h>
#include <util/sha.h>
#include <util/shell.h>
#include <util/subprocess.h>

#include <oci/auth.h>
#include <oci/types.h>
#include <uenv/parse.h>

#include "util.h"

namespace uenv {

util::expected<std::optional<oci::credentials>, std::string>
resolve_registry_credentials(const envvars::state& env,
                             const util::url& registry_url,
                             std::optional<std::string> username,
                             std::optional<std::string> token) {
    namespace fs = std::filesystem;

    // the credential store is keyed by the bare registry host[:port].
    const std::string host = registry_url.host_port();

    oci::credential_sources sources;
    if (token) {
        sources.explicit_token = fs::path{*token};
    }
    // the username that a bare token is paired with: --username, else the
    // calling environment. getlogin() is deliberately not consulted - it fails
    // when there is neither a controlling terminal nor an audit login uid, i.e.
    // in the batch step where a token-authenticated push runs.
    sources.username = username ? std::move(username) : envvars::user_name(env);

    // the uenv token store: $XDG_CONFIG_HOME/uenv/tokens or
    // ~/.config/uenv/tokens.
    if (auto xdg = env.get("XDG_CONFIG_HOME")) {
        sources.uenv_token_dir = fs::path{*xdg} / "uenv" / "tokens";
    } else if (auto home = env.get("HOME")) {
        sources.uenv_token_dir =
            fs::path{*home} / ".config" / "uenv" / "tokens";
    }

    // docker config.json fallback: $DOCKER_CONFIG/config.json or
    // ~/.docker/config.json (a missing file is handled gracefully downstream).
    if (auto dcfg = env.get("DOCKER_CONFIG")) {
        sources.docker_config = fs::path{*dcfg} / "config.json";
    } else if (auto home = env.get("HOME")) {
        sources.docker_config = fs::path{*home} / ".docker" / "config.json";
    }

    auto creds = oci::resolve_credentials(host, sources);
    // src/oci reports a token with no username in flag-free terms, because it
    // has no idea what this program's flags are called. Translating it is this
    // layer's job.
    if (!creds && creds.error() == oci::username_required_error) {
        return util::unexpected{
            "a token was found, but uenv could not determine your username: "
            "pass it with --username.\nThe username is taken from --username, "
            "or from $USER, which may be unset in a sanitised environment."};
    }
    return creds;
}

// returns true if squashfs-mount 9 or later is detected in PATH.
// this is used for a compatibility layer
bool sqfs_mount_v9(const envvars::state& calling_environment) {
    if (auto PATH = calling_environment.get("PATH")) {
        if (auto exe = util::which("squashfs-mount", *PATH)) {
            if (auto run = util::run({exe->string(), "--version"})) {
                if (!run->wait()) {
                    if (auto output = run->out.getline()) {
                        if (auto version = uenv::parse_semver(output.value())) {
                            spdlog::debug("squashfs-mount version {}",
                                          *version);
                            return version->major >= 9;
                        }
                    }
                }
            } else {
                spdlog::warn("squashfs-mount is not in PATH");
            }
        }
    }
    return false;
}

std::vector<std::string>
squashfs_mount_args(const envvars::state& calling_environment,
                    const std::vector<std::string>& mounts, bool join,
                    int verbosity, const std::vector<std::string>& args) {
    // explicitly set the full path of squashfs-mount, for better logging.
    std::string exe = util::which("squashfs-mount",
                                  calling_environment.get("PATH").value_or(""))
                          .value_or("squashfs-mount");
    std::vector<std::string> commands = {exe};
    if (join) {
        commands.push_back("--join");
    }

    if (verbosity > 0) {
        commands.push_back("-" + std::string(verbosity, 'v'));
    }

    if (!sqfs_mount_v9(calling_environment)) {
        for (auto& m : mounts) {
            commands.push_back(m);
        }
    } else {
        commands.push_back(fmt::format("--sqfs={}", fmt::join(mounts, ",")));
    }
    commands.push_back("--");
    commands.insert(commands.end(), args.begin(), args.end());
    spdlog::debug("{}", fmt::join(commands, " "));
    return commands;
}

namespace {
// bytes -> whole megabytes, rounded up.
std::size_t to_mb(std::uint64_t bytes) {
    constexpr std::uint64_t mb = 1024 * 1024;
    return static_cast<std::size_t>((bytes + mb - 1) / mb);
}
} // namespace

transfer_bar::transfer_bar(std::uint64_t total_bytes, std::string message)
    // never zero: barkeep divides by the total to render the bar.
    : total_mb_{std::max<std::size_t>(1u, to_mb(total_bytes))} {
    namespace bk = barkeep;
    bar_ = bk::ProgressBar(&transferred_mb_,
                           {
                               .total = total_mb_,
                               .message = std::move(message),
                               .speed = 0.1,
                               .speed_unit = "MB/s",
                               .style = color::use_color()
                                            ? bk::ProgressBarStyle::Rich
                                            : bk::ProgressBarStyle::Bars,
                               .no_tty = !isatty(fileno(stdout)),
                           });
}

void transfer_bar::update(std::uint64_t bytes) {
    transferred_mb_.store(to_mb(bytes), std::memory_order_relaxed);
}

void transfer_bar::finish() {
    transferred_mb_.store(total_mb_, std::memory_order_relaxed);
    bar_->done();
}

void transfer_bar::stop() {
    bar_->done();
}

std::unique_ptr<transfer_bar> make_transfer_bar(std::uint64_t total_bytes,
                                                std::string message) {
    return std::make_unique<transfer_bar>(total_bytes, std::move(message));
}

util::expected<squashfs_image, std::string> validate_squashfs_image(
    const std::string& path,
    std::function<void(std::uint64_t done, std::uint64_t total)>
        hash_progress) {
    namespace fs = std::filesystem;

    squashfs_image img{};

    auto file = uenv::parse_path(path);
    if (!file) {
        return util::unexpected{fmt::format("invalid squashfs file {}: {}",
                                            path, file.error().message())};
    }
    img.sqfs = *file;
    if (!fs::is_regular_file(img.sqfs)) {
        return util::unexpected{
            fmt::format("invalid squashfs: {} is not a file", path)};
    }
    img.sqfs = fs::absolute(img.sqfs);
    spdlog::info("found squashfs {}", img.sqfs);

    if (auto p = util::unsquashfs_tmp(img.sqfs, "meta")) {
        img.meta = p.value() / "meta";
    } else {
        spdlog::info("no meta data in {}", img.sqfs);
    }

    std::error_code ec;
    const auto total = fs::file_size(img.sqfs, ec);
    // report the total up front, so a caller's bar appears before the first
    // chunk is hashed rather than after it.
    if (hash_progress) {
        hash_progress(0u, ec ? 0u : total);
    }
    auto hash = util::sha256_file(img.sqfs, [&](std::uint64_t hashed) {
        if (hash_progress) {
            hash_progress(hashed, ec ? 0u : total);
        }
    });
    if (!hash) {
        spdlog::error("{}", hash.error());
        return util::unexpected{fmt::format(
            "unable to calculate sha256 of squashfs file {}", img.sqfs)};
    }
    img.hash = hash.value();

    return img;
}

} // namespace uenv
