#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include <uenv/oras.h>
#include <uenv/parse.h>
#include <uenv/registry.h>
#include <uenv/uenv.h>
#include <util/curl.h>
#include <util/expected.h>
#include <util/fs.h>

namespace uenv {

namespace impl {

util::expected<manifest, std::string>
manifest_from_json(const std::string& input) {
    manifest m{};
    try {
        auto json = nlohmann::json{input};
        m.json = input;
        m.size = json["layers"][0]["size"];
        m.digest = parse_oras_sha256(json["layers"][0]["digest"]).value();
    } catch (std::exception& e) {
        return util::unexpected{fmt::format("error parsing json {}", e.what())};
    }

    return m;
}

} // namespace impl

registry::registry(std::string url) : url(std::move(url)) {
}

util::expected<void, std::string>
registry::login(const oras::credentials& creds) {
    // oras login -u username -p password
    auto path = util::make_temp_dir();

    auto result = oras::login(url, creds, path);

    if (!result) {
        return util::unexpected{result.error().message};
    }

    config = cfg{.path = path, .credentials = creds};

    return {};
}

util::expected<registry_manifest, std::string>
registry::find(const uenv_label& label, std::optional<std::string> prefix) {
    auto base_url = prefix ? url : fmt::format("{}/{}", url, prefix.value());
    auto full_url =
        fmt::format("{}/{}/{}/{}/{}:{}", base_url, label.system.value(),
                    label.uarch.value(), label.name.value(),
                    label.version.value(), label.tag.value());

    // oras manifest fetch localhost:5862/deploy/cluster/zen3/app/1.0:v1
    auto result = config ? oras::fetch_manifest(full_url, config->path)
                         : oras::fetch_manifest(full_url);

    if (!result) {
        return util::unexpected{result.error().message};
    }

    manifest oci;
    if (auto p = impl::manifest_from_json(result.value())) {
        oci = p.value();
    } else {
        return util::unexpected{p.error()};
    }

    return registry_manifest{.oci = oci, .label = label, .url = base_url};
}

util::expected<void, std::string> registry::remove(const registry_manifest&) {
    if (!config) {
        return util::unexpected{
            "credentials must be provided to delete images from a repository"};
    }
    const auto& creds = config->credentials;
    if (auto result = util::curl::del(url, creds.username, creds.token);
        !result) {
        return util::unexpected{
            fmt::format("unable to delete uenv: {}", result.error().message)};
    }

    return {};
}

} // namespace uenv
