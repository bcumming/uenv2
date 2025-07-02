#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <uenv/uenv.h>
#include <util/expected.h>

namespace uenv {
namespace oras {

struct credentials {
    std::string username;
    std::string token;
};

struct error {
    error(std::string_view msg) : message(msg) {
    }
    error(int code, std::string_view err, std::string_view msg)
        : returncode(code), stderr(err), message(msg) {
    }
    error() = default;

    int returncode = 0;
    std::string stderr = {};
    std::string message = {};

    operator bool() const {
        return returncode == 0;
    };
};

bool pull(std::string rego, std::string nspace);

util::expected<std::vector<std::string>, error>
discover(const std::string& registry, const std::string& nspace,
         const uenv_record& uenv,
         const std::optional<credentials> token = std::nullopt);

util::expected<void, error>
pull_digest(const std::string& registry, const std::string& nspace,
            const uenv_record& uenv, const std::string& digest,
            const std::filesystem::path& destination,
            const std::optional<credentials> token = std::nullopt);

util::expected<void, error>
pull_tag(const std::string& registry, const std::string& nspace,
         const uenv_record& uenv, const std::filesystem::path& destination,
         const std::optional<credentials> token = std::nullopt);

util::expected<void, error>
push_tag(const std::string& registry, const std::string& nspace,
         const uenv_label& label, const std::filesystem::path& source,
         const std::optional<credentials> token = std::nullopt);

util::expected<void, error>
push_meta(const std::string& registry, const std::string& nspace,
          const uenv_label& label, const std::filesystem::path& meta_path,
          const std::optional<credentials> token = std::nullopt);

util::expected<void, error>
copy(const std::string& registry, const std::string& src_nspace,
     const uenv_record& src_uenv, const std::string& dst_nspace,
     const uenv_record& dst_uenv,
     const std::optional<credentials> token = std::nullopt);

util::expected<void, error> login(const std::string& url,
                                  const credentials& credentials,
                                  const std::filesystem::path& path);

util::expected<std::string, error> fetch_manifest(
    const std::string& url,
    const std::optional<std::filesystem::path>& credentials = std::nullopt);

} // namespace oras
} // namespace uenv

#include <fmt/core.h>
template <> class fmt::formatter<uenv::oras::credentials> {
  public:
    // parse format specification and store it:
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.end();
    }
    // format a value using stored specification:
    template <typename FmtContext>
    constexpr auto format(uenv::oras::credentials const& c,
                          FmtContext& ctx) const {
        // replace token characters with 'X'
        return fmt::format_to(ctx.out(), "{{username: {}, token: {:X>{}}}}",
                              c.username, "", c.token.size());
    }
};
