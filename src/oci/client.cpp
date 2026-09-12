#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <oci/client.h>
#include <oci/parse.h>
#include <oci/util.h>
#include <util/curl.h>
#include <util/expected.h>
#include <util/fs.h>
#include <util/sha.h>
#include <util/url.h>

namespace oci {

//
// client_impl
//

// The registry connection's state and all of its HTTP machinery, held behind
// client's pimpl so that util/curl.h never reaches the public header. Every
// public client method is a forwarder onto the method of the same name here.
class client_impl {
  public:
    // there is no default constructor: registry_url_ is a util::url, which has
    // no empty state, so a client_impl is only ever built fully-formed.
    client_impl(util::url registry_url, std::string repository,
                std::optional<credentials> creds,
                std::optional<bearer_challenge> challenge)
        : registry_url_(std::move(registry_url)),
          repository_(std::move(repository)), creds_(std::move(creds)),
          challenge_(std::move(challenge)) {
    }

    // does a blob exist? HEAD; 200 -> true, 404 -> false.
    util::expected<bool, client_error> blob_exists(const digest& d);

    util::expected<void, client_error>
    get_blob_to_file(const digest& d, const std::filesystem::path& file,
                     std::function<void(std::uint64_t, std::uint64_t)> progress,
                     std::function<bool()> should_abort);

    util::expected<manifest_response, client_error>
    get_manifest(const reference& ref);

    util::expected<std::vector<std::string>, client_error> list_tags();

    util::expected<std::vector<descriptor>, client_error>
    referrers(const digest& d);

    util::expected<bool, client_error>
    mount_blob(const digest& d, const std::string& from_repository);

    util::expected<void, client_error>
    put_blob(const digest& d, const std::filesystem::path& file,
             std::function<void(std::uint64_t, std::uint64_t)> progress);

    util::expected<void, client_error> put_blob_bytes(const digest& d,
                                                      const std::string& data);

    util::expected<void, client_error>
    put_manifest(const reference& ref, const std::string& body,
                 std::string_view media_type);

    void add_pull_scope(std::string repository);

    // a cached bearer token and the deadline after which it is considered
    // stale (absent when the token endpoint did not advertise a lifetime).
    struct cached_token {
        std::string value;
        std::optional<std::chrono::steady_clock::time_point> expires_at;
    };

    // refresh a cached token this long before its advertised expiry, so a
    // token that is about to lapse is not used to start a new request.
    static constexpr std::chrono::seconds token_refresh_margin{30};

    // return a bearer token for this repository, fetching/caching as needed;
    // a cached token past (or within token_refresh_margin of) its advertised
    // expiry is refetched. returns std::nullopt for an anonymous registry (no
    // auth challenge), in which case requests are sent without an
    // Authorization header.
    util::expected<std::optional<std::string>, std::string>
    token_for(bool write);

    // set the bearer token on `req` and perform it. if the registry rejects
    // a previously cached token with 401 — typically one whose lifetime ran
    // out during a preceding long transfer, without the token endpoint having
    // advertised expires_in — a fresh token is fetched and the request
    // retried once; `reset` (when set) runs before the retry so the caller
    // can rewind per-attempt state (e.g. a streaming hash). a 401 on a
    // freshly fetched token is a real denial and is returned as-is.
    util::expected<util::curl::response, client_error>
    authed_perform(util::curl::request& req, bool write,
                   const std::function<void()>& reset = {});

    // read the referrers of `d` from the referrers tag schema index
    // (<algo>-<hex>): the fallback for registries without the Referrers API.
    util::expected<std::vector<descriptor>, client_error>
    referrers_from_tag(const digest& d);

    // upload a blob via the monolithic POST-then-PUT handshake; `body_setup`
    // attaches the payload (a file to stream, or an in-memory body) to the
    // PUT request.
    util::expected<void, client_error>
    put_blob_impl(const std::string& digest,
                  const std::function<void(util::curl::request&)>& body_setup);

    // origin() already: scheme://host[:port], nothing to strip.
    util::url registry_url_;
    std::string repository_;
    std::optional<credentials> creds_;
    // the auth challenge, or nullopt when the registry permits anonymous
    // access.
    std::optional<bearer_challenge> challenge_;
    std::optional<cached_token> pull_token_;
    std::optional<cached_token> push_token_;
    // extra repositories to request pull scope for (cross-repo mount).
    std::vector<std::string> extra_pull_scopes_;
};

util::expected<client, client_error>
client::create(util::url registry_url, std::string repository,
               std::optional<credentials> creds) {
    auto challenge = discover_challenge(registry_url);
    if (!challenge) {
        return util::unexpected{client_error{challenge.error()}};
    }

    return client{std::make_unique<client_impl>(
        registry_url.origin(), std::move(repository), std::move(creds),
        std::move(*challenge))};
}

util::expected<std::optional<std::string>, std::string>
client_impl::token_for(bool write) {
    // anonymous registry: no challenge, so no token is needed or fetched.
    if (!challenge_) {
        return std::nullopt;
    }
    auto& cache = write ? push_token_ : pull_token_;
    if (cache) {
        if (!cache->expires_at ||
            std::chrono::steady_clock::now() <
                cache->expires_at.value() - token_refresh_margin) {
            return cache->value;
        }
        spdlog::debug("oci::token_for cached {} token reached its advertised "
                      "expiry, refreshing",
                      write ? "push" : "pull");
        cache = std::nullopt;
    }
    std::vector<std::string> scopes;
    scopes.push_back(
        detail::repository_scope(repository_, write ? "pull,push" : "pull"));
    // cross-repo blob mounts need pull scope on the source repository too.
    for (const auto& r : extra_pull_scopes_) {
        scopes.push_back(detail::repository_scope(r, "pull"));
    }
    auto token = fetch_token(registry_url_, *challenge_, scopes, creds_);
    if (!token) {
        return util::unexpected{token.error()};
    }
    cached_token entry{.value = std::move(token->token),
                       .expires_at = std::nullopt};
    if (token->expires_in) {
        entry.expires_at = std::chrono::steady_clock::now() +
                           std::chrono::seconds{token->expires_in.value()};
        spdlog::debug("oci::token_for fetched {} token (expires in {}s)",
                      write ? "push" : "pull", token->expires_in.value());
    } else {
        spdlog::debug("oci::token_for fetched {} token (no advertised expiry)",
                      write ? "push" : "pull");
    }
    cache = std::move(entry);
    return cache->value;
}

util::expected<util::curl::response, client_error>
client_impl::authed_perform(util::curl::request& req, bool write,
                            const std::function<void()>& reset) {
    auto& cache = write ? push_token_ : pull_token_;
    const bool was_cached = cache.has_value();
    auto token = token_for(write);
    if (!token) {
        return util::unexpected{client_error{token.error()}};
    }
    req.bearer_token = *token;

    auto resp = util::curl::perform(req);

    // a 401 on a token that was fetched for an earlier request usually means
    // it expired in between (e.g. during a transfer that outlived the token's
    // lifetime): fetch a fresh token and retry once.
    if (resp && resp->status == 401 && was_cached && token->has_value()) {
        spdlog::debug("oci::authed_perform 401 with a cached token: "
                      "refreshing the token and retrying {}",
                      req.url);
        cache = std::nullopt;
        token = token_for(write);
        if (!token) {
            return util::unexpected{client_error{token.error()}};
        }
        req.bearer_token = *token;
        if (reset) {
            reset();
        }
        resp = util::curl::perform(req);
    }

    if (!resp) {
        return util::unexpected{client_error{resp.error().message}};
    }
    return *resp;
}

void client_impl::add_pull_scope(std::string repository) {
    extra_pull_scopes_.push_back(std::move(repository));
}

util::expected<bool, client_error> client_impl::blob_exists(const digest& d) {
    util::curl::request req;
    req.url = registry_url_.resolve(detail::blob_path(repository_, d.string()))
                  .string();
    req.method = util::curl::http_method::head;

    auto resp = authed_perform(req, false);
    if (!resp) {
        return util::unexpected{resp.error()};
    }
    if (resp->status == 200) {
        return true;
    }
    if (resp->status == 404) {
        return false;
    }
    return util::unexpected{
        client_error{fmt::format("unexpected status {} for HEAD {}",
                                 resp->status, d.string()),
                     resp->status}};
}

util::expected<void, client_error> client_impl::get_blob_to_file(
    const digest& d, const std::filesystem::path& file,
    std::function<void(std::uint64_t, std::uint64_t)> progress,
    std::function<bool()> should_abort) {
    // check the destination directory is writable up front, so a bad target
    // surfaces as a clear error rather than a curl write failure.
    const auto parent = file.parent_path();
    if (!parent.empty() &&
        util::file_access_level(parent) != util::file_level::readwrite) {
        return util::unexpected{client_error{fmt::format(
            "cannot write blob to {}: {} is not a writable directory",
            file.string(), parent.string())}};
    }

    // stream to a temporary name and rename into place only after the
    // transfer and digest verification succeed: a failed or interrupted
    // download must never leave a file at the final path, where a later
    // caller would mistake it for a complete blob.
    const std::filesystem::path partial{file.string() + ".partial"};

    util::curl::request req;
    req.url = registry_url_.resolve(detail::blob_path(repository_, d.string()))
                  .string();
    req.follow_redirects = true;
    req.download_file = partial;
    req.on_download_progress = std::move(progress);
    req.should_abort = std::move(should_abort);

    // hash the blob as it streams to disk so its content can be verified
    // against the requested digest without a second read pass over a multi-GB
    // file.
    const bool verify = d.algorithm() == "sha256";
    util::sha256_hasher hash;
    if (verify) {
        req.on_download_data = [&hash](const char* p, std::size_t n) {
            hash.update(std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(p), n});
        };
    }

    // every error path must remove the partial download before surfacing
    // the error.
    auto fail = [&partial](client_error err) {
        spdlog::debug("oci::get_blob_to_file removing partial download {}",
                      partial.string());
        std::error_code ec;
        std::filesystem::remove(partial, ec);
        if (ec) {
            spdlog::warn("oci::get_blob_to_file unable to remove {}: {}",
                         partial.string(), ec.message());
        }
        return util::unexpected{std::move(err)};
    };

    spdlog::debug("oci::get_blob_to_file downloading {} to {}", req.url,
                  partial.string());
    // on a 401-retry the hash must restart from scratch: the retried request
    // rewrites the partial file from the beginning ("wb" truncates it).
    auto resp = authed_perform(req, false, [verify, &hash]() {
        if (verify) {
            hash.reset();
        }
    });
    if (!resp) {
        return fail(resp.error());
    }
    if (resp->status != 200) {
        return fail(
            {fmt::format("failed to fetch blob {} (status {}): {}", d.string(),
                         resp->status, util::curl::http_message(resp->status)),
             resp->status});
    }

    if (verify) {
        const auto got = digest::sha256(hash.finalize());
        if (got != d) {
            return fail(
                {fmt::format("downloaded blob digest mismatch: expected {}, "
                             "got {}",
                             d.string(), got.string())});
        }
        spdlog::debug("oci::get_blob_to_file verified {}", got.string());
    }

    spdlog::debug("oci::get_blob_to_file moving {} to {}", partial.string(),
                  file.string());
    std::error_code ec;
    std::filesystem::rename(partial, file, ec);
    if (ec) {
        return fail(
            {fmt::format("unable to move downloaded blob {} to {}: {}",
                         partial.string(), file.string(), ec.message())});
    }
    return {};
}

util::expected<manifest_response, client_error>
client_impl::get_manifest(const reference& ref) {
    // a manifest whose digest cannot be computed cannot be verified, so it is
    // never fetched: an unsupported algorithm is an error rather than a
    // response that silently skips the check below.
    if (ref.is_digest() && ref.as_digest().algorithm() != "sha256") {
        return util::unexpected{client_error{fmt::format(
            "unable to verify manifest {}: unsupported digest algorithm '{}'",
            ref.string(), ref.as_digest().algorithm())}};
    }

    util::curl::request req;
    req.url =
        registry_url_.resolve(detail::manifest_path(repository_, ref.string()))
            .string();
    req.header_lines = {fmt::format("Accept: {}", media_type_manifest),
                        fmt::format("Accept: {}", media_type_index)};

    auto resp = authed_perform(req, false);
    if (!resp) {
        return util::unexpected{resp.error()};
    }
    if (resp->status != 200) {
        return util::unexpected{client_error{
            fmt::format("failed to fetch manifest {} (status {}): {}",
                        ref.string(), resp->status,
                        util::curl::http_message(resp->status)),
            resp->status}};
    }
    // A manifest's identity is the hash of its bytes, so it is computed here
    // and never taken from the Docker-Content-Digest header, which is only the
    // registry's assertion about its own response. Everything downstream hangs
    // off this: the layer digests that get_blob_to_file verifies against are
    // read out of this body, so a manifest taken on trust makes every check
    // below it self-referential.
    const auto computed = digest::sha256(util::sha256_string(resp->body));

    // A digest reference is a pin — uenv identifies an image by its manifest
    // digest — so bytes that hash to anything else are content the caller did
    // not ask for, whatever the registry says about them.
    if (ref.is_digest() && ref.as_digest() != computed) {
        return util::unexpected{client_error{
            fmt::format("manifest digest mismatch: requested {}, but the "
                        "registry returned content with digest {}",
                        ref.as_digest().string(), computed.string())}};
    }
    if (auto header = resp->headers.get("docker-content-digest")) {
        if (*header != computed.string()) {
            spdlog::warn("the registry reported manifest {} as {}, which does "
                         "not match the content it returned ({})",
                         ref.string(), *header, computed.string());
        }
    }
    spdlog::debug("oci::get_manifest {} has digest {}, computed over the "
                  "response body",
                  ref.string(), computed.string());

    return manifest_response{
        .body = std::move(resp->body),
        .digest = computed,
        .media_type = resp->headers.get("content-type").value_or("")};
}

util::expected<std::vector<std::string>, client_error>
client_impl::list_tags() {
    util::curl::request req;
    req.url = registry_url_.resolve(detail::tags_path(repository_)).string();

    auto resp = authed_perform(req, false);
    if (!resp) {
        return util::unexpected{resp.error()};
    }
    if (resp->status != 200) {
        return util::unexpected{client_error{
            fmt::format("failed to list tags (status {})", resp->status),
            resp->status}};
    }
    auto tags = detail::parse_tags_list(resp->body);
    if (!tags) {
        return util::unexpected{
            client_error{"could not parse tags/list response"}};
    }
    return *tags;
}

util::expected<std::vector<descriptor>, client_error>
client_impl::referrers(const digest& d) {
    util::curl::request req;
    req.url =
        registry_url_.resolve(detail::referrers_path(repository_, d.string()))
            .string();

    auto resp = authed_perform(req, false);
    if (!resp) {
        return util::unexpected{resp.error()};
    }
    if (resp->status == 404) {
        // per the OCI 1.1 distribution spec, a 404 means the registry does
        // not implement the Referrers API (implementing registries return 200
        // with an empty index, even for a subject with no referrers). fall
        // back to the referrers tag schema that the push side maintains.
        spdlog::debug("oci::referrers {}: Referrers API unavailable (404), "
                      "falling back to the referrers tag schema",
                      d.string());
        return referrers_from_tag(d);
    }
    if (resp->status != 200) {
        return util::unexpected{client_error{
            fmt::format("failed to fetch referrers of {} (status {}): {}",
                        d.string(), resp->status,
                        util::curl::http_message(resp->status)),
            resp->status}};
    }
    auto refs = detail::parse_referrers(resp->body);
    if (!refs) {
        return util::unexpected{
            client_error{"could not parse referrers response"}};
    }
    return *refs;
}

util::expected<std::vector<descriptor>, client_error>
client_impl::referrers_from_tag(const digest& d) {
    // the tag schema names an OCI image index <algo>-<hex> in the same
    // repository as the subject (see maintain_referrers_tag in push.cpp).
    const auto tag = d.algorithm() + "-" + d.hex();
    util::curl::request req;
    req.url =
        registry_url_.resolve(detail::manifest_path(repository_, tag)).string();
    req.header_lines = {fmt::format("Accept: {}", media_type_index),
                        fmt::format("Accept: {}", media_type_manifest)};

    spdlog::debug("oci::referrers_from_tag fetching {}", req.url);
    auto resp = authed_perform(req, false);
    if (!resp) {
        return util::unexpected{resp.error()};
    }
    if (resp->status == 404) {
        // the tag was never created: the subject has no referrers.
        spdlog::debug("oci::referrers_from_tag {}: tag absent, no referrers",
                      tag);
        return std::vector<descriptor>{};
    }
    if (resp->status != 200) {
        return util::unexpected{client_error{
            fmt::format("failed to fetch referrers tag {} (status {}): {}", tag,
                        resp->status, util::curl::http_message(resp->status)),
            resp->status}};
    }
    auto refs = detail::parse_referrers(resp->body);
    if (!refs) {
        return util::unexpected{client_error{
            fmt::format("could not parse referrers tag index {}", tag)}};
    }
    spdlog::debug("oci::referrers_from_tag {}: {} referrer(s)", tag,
                  refs->size());
    return *refs;
}

// drive the monolithic upload handshake: POST an upload session, then PUT the
// payload (carried by `body_setup`) to the returned Location with ?digest=.
util::expected<void, client_error> client_impl::put_blob_impl(
    const std::string& digest,
    const std::function<void(util::curl::request&)>& body_setup) {
    // 1. open an upload session
    util::curl::request post;
    post.url =
        registry_url_.resolve(detail::uploads_path(repository_)).string();
    post.method = util::curl::http_method::post;
    auto opened = authed_perform(post, true);
    if (!opened) {
        return util::unexpected{opened.error()};
    }
    if (opened->status != 202) {
        return util::unexpected{client_error{
            fmt::format("failed to open upload session (status {}): {} {}",
                        opened->status,
                        util::curl::http_message(opened->status), opened->body),
            opened->status}};
    }
    auto location = opened->headers.get("location");
    if (!location) {
        return util::unexpected{
            client_error{"upload session response had no Location header"}};
    }

    // 2. PUT the payload to <location>?digest=<digest>
    auto put_url = detail::resolve_upload_url(registry_url_, *location, digest);
    if (!put_url) {
        return util::unexpected{client_error{put_url.error()}};
    }
    util::curl::request put;
    put.url = put_url->string();
    put.method = util::curl::http_method::put;
    put.header_lines = {
        fmt::format("Content-Type: {}", media_type_octet_stream)};
    body_setup(put);

    auto done = authed_perform(put, true);
    if (!done) {
        return util::unexpected{done.error()};
    }
    if (done->status != 201) {
        return util::unexpected{client_error{
            fmt::format("failed to upload blob {} (status {}): {} {}", digest,
                        done->status, util::curl::http_message(done->status),
                        done->body),
            done->status}};
    }
    return {};
}

util::expected<bool, client_error>
client_impl::mount_blob(const digest& d, const std::string& from_repository) {
    if (auto exists = blob_exists(d); exists && *exists) {
        return true;
    }
    util::curl::request post;
    post.url = registry_url_.resolve(detail::uploads_path(repository_))
                   .query_param("mount", d.string())
                   .query_param("from", from_repository)
                   .string();
    post.method = util::curl::http_method::post;

    auto resp = authed_perform(post, true);
    if (!resp) {
        return util::unexpected{resp.error()};
    }
    // 201: the blob was mounted. 202: the registry declined and opened an
    // upload session instead (caller must copy the blob the slow way).
    if (resp->status == 201) {
        return true;
    }
    if (resp->status == 202) {
        return false;
    }
    return util::unexpected{client_error{
        fmt::format("failed to mount blob {} from {} (status {}): {}",
                    d.string(), from_repository, resp->status, resp->body),
        resp->status}};
}

util::expected<void, client_error> client_impl::put_blob(
    const digest& d, const std::filesystem::path& file,
    std::function<void(std::uint64_t, std::uint64_t)> progress) {
    if (util::file_access_level(file) < util::file_level::readonly) {
        return util::unexpected{client_error{fmt::format(
            "cannot upload blob: {} is not a readable file", file.string())}};
    }
    if (auto exists = blob_exists(d); exists && *exists) {
        spdlog::trace("oci::put_blob {} already present", d.string());
        return {};
    }
    return put_blob_impl(d.string(),
                         [&file, &progress](util::curl::request& r) {
                             r.upload_file = file;
                             if (progress) {
                                 r.on_upload_progress = progress;
                             }
                         });
}

util::expected<void, client_error>
client_impl::put_blob_bytes(const digest& d, const std::string& data) {
    if (auto exists = blob_exists(d); exists && *exists) {
        return {};
    }
    return put_blob_impl(d.string(),
                         [&data](util::curl::request& r) { r.body = data; });
}

util::expected<void, client_error>
client_impl::put_manifest(const reference& ref, const std::string& body,
                          std::string_view media_type) {
    util::curl::request req;
    req.url =
        registry_url_.resolve(detail::manifest_path(repository_, ref.string()))
            .string();
    req.method = util::curl::http_method::put;
    req.body = body;
    req.header_lines = {fmt::format("Content-Type: {}", media_type)};

    auto resp = authed_perform(req, true);
    if (!resp) {
        return util::unexpected{resp.error()};
    }
    if (resp->status != 201) {
        return util::unexpected{client_error{
            fmt::format("failed to put manifest {} (status {}): {} {}",
                        ref.string(), resp->status,
                        util::curl::http_message(resp->status), resp->body),
            resp->status}};
    }
    return {};
}

//
// client: forwarders onto client_impl
//

client::client(std::unique_ptr<client_impl> impl) : impl_(std::move(impl)) {
}

client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

util::expected<bool, client_error> client::blob_exists(const digest& d) {
    return impl_->blob_exists(d);
}

util::expected<void, client_error> client::get_blob_to_file(
    const digest& d, const std::filesystem::path& file,
    std::function<void(std::uint64_t, std::uint64_t)> progress,
    std::function<bool()> should_abort) {
    return impl_->get_blob_to_file(d, file, std::move(progress),
                                   std::move(should_abort));
}

util::expected<manifest_response, client_error>
client::get_manifest(const reference& ref) {
    return impl_->get_manifest(ref);
}

util::expected<std::vector<std::string>, client_error> client::list_tags() {
    return impl_->list_tags();
}

util::expected<std::vector<descriptor>, client_error>
client::referrers(const digest& d) {
    return impl_->referrers(d);
}

util::expected<bool, client_error>
client::mount_blob(const digest& d, const std::string& from_repository) {
    return impl_->mount_blob(d, from_repository);
}

util::expected<void, client_error>
client::put_blob(const digest& d, const std::filesystem::path& file,
                 std::function<void(std::uint64_t, std::uint64_t)> progress) {
    return impl_->put_blob(d, file, std::move(progress));
}

util::expected<void, client_error>
client::put_blob_bytes(const digest& d, const std::string& data) {
    return impl_->put_blob_bytes(d, data);
}

util::expected<void, client_error>
client::put_manifest(const reference& ref, const std::string& body,
                     std::string_view media_type) {
    return impl_->put_manifest(ref, body, media_type);
}

void client::add_pull_scope(std::string repository) {
    impl_->add_pull_scope(std::move(repository));
}

} // namespace oci
