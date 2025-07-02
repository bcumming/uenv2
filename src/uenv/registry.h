#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <uenv/oras.h>
#include <uenv/uenv.h>
#include <util/expected.h>

namespace uenv {

struct manifest {
    sha256 digest;
    std::size_t size;
    std::string json;
};

// represents a concrete artifact in repository
struct repo_manifest {
    manifest oci;
    uenv_label label;
    std::filesystem::path path;
};

// represents a concrete artifact in registry
struct registry_manifest {
    manifest oci;
    uenv_label label;
    std::string url;
};

struct registry {
  private:
    struct cfg {
        std::filesystem::path path;
        oras::credentials credentials;
    };

    std::optional<cfg> config;
    std::string url;

  public:
    registry(std::string url);

    // login
    util::expected<void, std::string> login(const oras::credentials&);

    // return a list of manifests that match the search term
    util::expected<registry_manifest, std::string>
    find(const uenv_label& label, std::optional<std::string> prefix = {});

    // delete a manifest from the registry
    util::expected<void, std::string> remove(const registry_manifest&);
};

} // namespace uenv
