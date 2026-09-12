#pragma once

// The OCI vocabulary that does not need a registry connection: media types,
// content descriptors, registry addressing and the raw result of a manifest
// fetch. These live apart from client.h so that a consumer which only speaks
// about OCI content (manifest.h, the response parsers in util.h) does not have
// to see the client — and, through it, libcurl.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <oci/digest.h>
#include <util/expected.h>
#include <util/parse.h>
#include <util/url.h>

namespace oci {

// common OCI media types
inline constexpr std::string_view media_type_manifest =
    "application/vnd.oci.image.manifest.v1+json";
inline constexpr std::string_view media_type_index =
    "application/vnd.oci.image.index.v1+json";
inline constexpr std::string_view media_type_octet_stream =
    "application/octet-stream";

// An OCI content descriptor (a manifest entry / referrer record). A descriptor
// is not default-constructible: it must carry a valid `digest`, so a descriptor
// value is always well-formed.
struct descriptor {
    std::string media_type;
    oci::digest digest;
    std::size_t size = 0;
    std::optional<std::string> artifact_type;
    // inline base64 content (`data`), present on the empty config descriptor.
    std::optional<std::string> data;

    friend bool operator==(const descriptor&, const descriptor&) = default;
};

// A registry address split into a base url and a repository path prefix.
struct registry_location {
    util::url base;     // e.g. "https://jfrog.svc.cscs.ch"
    std::string prefix; // e.g. "uenv" (may be empty)
};

// Split a configured registry url into the base to address and the repository
// prefix: the base is its origin (scheme://host[:port]) and the prefix is its
// path, stripped of the surrounding slashes. Cannot fail - the url was parsed
// when it was read (see uenv::registry_config).
registry_location split_registry(const util::url& configured_url);

// Build the OCI repository path for a uenv, e.g.
// "<prefix>/<nspace>/<system>/<uarch>/<name>/<version>" (the prefix segment is
// omitted when empty). This mirrors the address oras formed.
std::string repository_path(std::string_view prefix, std::string_view nspace,
                            std::string_view system, std::string_view uarch,
                            std::string_view name, std::string_view version);

// The result of fetching a manifest: the raw bytes plus their locally computed
// digest and the media type.
struct manifest_response {
    std::string body;
    // the digest of `body`, computed locally by client::get_manifest. never
    // the Docker-Content-Digest header: that is the registry's assertion
    // about its own response, so taking it as the manifest's identity would
    // let a registry name content it did not send. a manifest fetched by
    // digest has already been checked against the requested digest, so this
    // is equal to it.
    oci::digest digest;
    std::string media_type;
};

} // namespace oci
