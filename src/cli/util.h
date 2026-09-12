#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <barkeep/barkeep.h>
#include <fmt/format.h>

#include <util/envvars.h>
#include <util/expected.h>
#include <util/sha.h>

#include <oci/auth.h>
#include <util/url.h>

namespace uenv {

// Resolve registry credentials for the registry named by `registry_url`, trying
// (in order) an explicit --token, the uenv token store
// ($XDG_CONFIG_HOME/uenv/tokens/<host>), and ~/.docker/config.json. Returns
// std::nullopt for anonymous access. This assembles the uenv/environment paths
// and delegates to oci::resolve_credentials.
util::expected<std::optional<oci::credentials>, std::string>
resolve_registry_credentials(const envvars::state& env,
                             const util::url& registry_url,
                             std::optional<std::string> username,
                             std::optional<std::string> token);

// A progress bar over a byte transfer: hashing, uploading or downloading a
// squashfs image. Progress is reported and displayed in whole megabytes.
//
// barkeep polls the counter through a pointer, from its own display thread, so
// this owns the counter and must never be copied or moved - doing so would
// leave that thread reading a dangling address. Construct one with
// make_transfer_bar, which keeps it at a stable address on the heap.
class transfer_bar {
  public:
    transfer_bar(std::uint64_t total_bytes, std::string message);

    transfer_bar(const transfer_bar&) = delete;
    transfer_bar& operator=(const transfer_bar&) = delete;
    transfer_bar(transfer_bar&&) = delete;
    transfer_bar& operator=(transfer_bar&&) = delete;

    // report the cumulative number of bytes transferred so far.
    void update(std::uint64_t bytes);

    // fill the bar and stop it. Also covers a transfer that was skipped
    // entirely, e.g. a blob the registry already had.
    void finish();

    // stop the bar where it is, for a transfer that was aborted.
    void stop();

  private:
    std::atomic<std::size_t> transferred_mb_{0u};
    std::size_t total_mb_;
    std::shared_ptr<barkeep::BaseDisplay> bar_;
};

std::unique_ptr<transfer_bar> make_transfer_bar(std::uint64_t total_bytes,
                                                std::string message);

struct squashfs_image {
    // the absolute path of the squashfs file
    std::filesystem::path sqfs;

    // a temporary path containing the meta data
    std::optional<std::filesystem::path> meta;

    // the sha256 hash of the file
    util::sha256 hash;
};

// Hashing the squashfs reads the whole file, which takes minutes for a
// multi-GB image. `hash_progress`, when set, is called with the cumulative
// bytes hashed and the total, so a caller can drive a progress bar. It is
// called once with (0, total) before hashing begins.
util::expected<squashfs_image, std::string> validate_squashfs_image(
    const std::string& path,
    std::function<void(std::uint64_t done, std::uint64_t total)> hash_progress =
        {});

std::vector<std::string>
squashfs_mount_args(const envvars::state& calling_environment,
                    const std::vector<std::string>& mounts, bool join,
                    int verbosity, const std::vector<std::string>& args);

} // namespace uenv

template <> class fmt::formatter<uenv::squashfs_image> {
  public:
    // parse format specification and store it:
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.end();
    }
    // format a value using stored specification:
    template <typename FmtContext>
    constexpr auto format(uenv::squashfs_image const& img,
                          FmtContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "squashfs_file(path {}, meta {}, hash {})",
                              img.sqfs, img.meta, img.hash);
    }
};
