#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <oci/client.h>
#include <oci/digest.h>
#include <oci/manifest.h>
#include <oci/push.h>
#include <oci/reference.h>
#include <oci/util.h>
#include <util/archive.h>
#include <util/expected.h>
#include <util/fs.h>
#include <util/sha.h>

namespace oci {

namespace fs = std::filesystem;

namespace {

util::expected<digest, std::string> digest_of_file(const fs::path& p) {
    auto d = util::sha256_file(p);
    if (!d) {
        return util::unexpected{d.error()};
    }
    return digest::sha256(*d);
}

digest digest_of_string(std::string_view s) {
    return digest::sha256(util::sha256_string(s));
}

// upload the canonical empty config blob (idempotent).
util::expected<void, client_error> put_empty_config(client& c) {
    return c.put_blob_bytes(empty_config_descriptor().digest,
                            std::string{empty_config_body});
}

// A packaged payload ready to become a manifest layer: the blob on disk plus
// the layer descriptor, and a scratch directory to clean up after upload.
struct packaged_layer {
    fs::path blob;
    manifest_layer layer;
    std::optional<fs::path> scratch = std::nullopt;
};

// Pack a directory into a deterministic gzipped tar, mirroring what
// `oras attach` produces for a directory payload: a single tar+gzip layer
// annotated for unpacking, titled with the directory name, and carrying the
// uncompressed-tar digest. util::pack_directory_targz builds a reproducible
// archive with no trailing padding past the tar EOF marker, which oras requires
// (see util/archive.cpp for the details).
util::expected<packaged_layer, std::string>
package_directory(const fs::path& dir) {
    const auto name = dir.filename().string();

    auto work = util::make_temp_dir();
    if (!work) {
        return util::unexpected{work.error()};
    }

    auto packed = util::pack_directory_targz(dir, *work);
    if (!packed) {
        return util::unexpected{packed.error()};
    }

    return packaged_layer{
        .blob = packed->gz_path,
        .layer =
            manifest_layer{
                .media_type = std::string{media_type_layer_targz},
                .digest = digest::sha256(packed->gz_sha256),
                .size = packed->gz_size,
                .annotations = {{std::string{annotation_content_digest},
                                 digest::sha256(packed->tar_sha256).string()},
                                {std::string{annotation_unpack}, "true"},
                                {std::string{annotation_title}, name}}},
        .scratch = *work};
}

// Package a single file as a raw blob layer (no unpack), titled with its
// filename, with a media type guessed from the extension.
util::expected<packaged_layer, std::string> package_file(const fs::path& file) {
    auto layer_digest = digest_of_file(file);
    if (!layer_digest) {
        return util::unexpected{layer_digest.error()};
    }
    std::error_code ec;
    auto size = fs::file_size(file, ec);
    if (ec) {
        return util::unexpected{
            fmt::format("unable to stat {}: {}", file.string(), ec.message())};
    }

    std::string media_type{media_type_octet_stream};
    if (file.extension() == ".json") {
        media_type = "application/json";
    }

    return packaged_layer{
        .blob = file,
        .layer = manifest_layer{.media_type = media_type,
                                .digest = *layer_digest,
                                .size = size,
                                .annotations = {{std::string{annotation_title},
                                                 file.filename().string()}}}};
}

// the blob digests a manifest references (config + layers).
std::vector<digest> blob_digests(const manifest& m) {
    std::vector<digest> out;
    out.push_back(m.config.digest);
    for (const auto& l : m.layers) {
        out.push_back(l.digest);
    }
    return out;
}

// copy one blob from `from_repo` into `dst`'s repository, preferring a
// server-side cross-repo mount and falling back to streaming it through a local
// temp file when the registry declines the mount.
util::expected<void, std::string> copy_blob(client& src, client& dst,
                                            const std::string& from_repo,
                                            const digest& d) {
    auto mounted = dst.mount_blob(d, from_repo);
    if (!mounted) {
        return util::unexpected{mounted.error().message};
    }
    if (*mounted) {
        spdlog::trace("copy_blob mounted {}", d.string());
        return {};
    }

    // fallback: pull the blob to a scratch file, then push it.
    spdlog::debug("copy_blob streaming {} (mount declined)", d.string());
    auto work = util::make_temp_dir();
    if (!work) {
        return util::unexpected{work.error()};
    }
    const auto tmp = *work / "blob";
    if (auto ok = src.get_blob_to_file(d, tmp); !ok) {
        return util::unexpected{ok.error().message};
    }
    if (auto ok = dst.put_blob(d, tmp); !ok) {
        return util::unexpected{ok.error().message};
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    return {};
}

// After a referrer manifest is pushed, keep the referrers-tag index
// (<algo>-<subject-hex>) up to date, for registries that do not auto-index the
// Referrers API. Best-effort: failures are logged, not fatal.
void maintain_referrers_tag(client& c, const digest& subject,
                            const descriptor& referrer) {
    // the referrers-index tag is "<algo>-<hex>", valid by construction.
    const auto tag = reference::tag(
        oci::tag::parse(subject.algorithm() + "-" + subject.hex()).value());

    // read the existing index (an OCI image index, i.e. a manifests[] list) so
    // we merge rather than clobber prior referrers.
    std::vector<descriptor> manifests;
    auto existing = c.get_manifest(tag);
    if (existing) {
        if (auto refs = detail::parse_referrers(existing->body)) {
            manifests = std::move(*refs);
        } else {
            // 200 with an unparseable body: the index is corrupt and useless
            // to every client, so rebuilding it loses nothing.
            spdlog::warn("referrers tag {} holds an unparseable index; "
                         "rebuilding it",
                         tag.string());
        }
    } else if (existing.error().http_status == 404) {
        // the tag has never been created: start a new index.
        spdlog::debug("referrers tag {} not found, creating it", tag.string());
    } else {
        // any other failure (transport, auth, 5xx) says nothing about the
        // tag's contents: it may hold referrers that rebuilding from scratch
        // would silently discard, so leave it unchanged.
        spdlog::warn("unable to read referrers tag {} ({}); leaving it "
                     "unchanged",
                     tag.string(), existing.error().message);
        return;
    }
    for (const auto& d : manifests) {
        if (d.digest == referrer.digest) {
            return; // already indexed
        }
    }
    manifests.push_back(referrer);

    auto body = serialize_index(manifests);
    if (auto put = c.put_manifest(tag, body, media_type_index); !put) {
        spdlog::warn("unable to update referrers tag {}: {}", tag.string(),
                     put.error());
    }
}

} // namespace

util::expected<digest, std::string>
push_squashfs(client& c, const fs::path& squashfs, const reference& ref,
              std::optional<digest> layer_digest,
              std::function<void(std::uint64_t, std::uint64_t)> progress,
              std::optional<std::string> existing_manifest_body) {
    // hashing a multi-GB squashfs is expensive, so a caller that already has
    // the digest passes it in rather than paying for a second pass.
    if (!layer_digest) {
        auto computed = digest_of_file(squashfs);
        if (!computed) {
            return util::unexpected{computed.error()};
        }
        layer_digest = computed.value();
    }
    std::error_code ec;
    auto size = fs::file_size(squashfs, ec);
    if (ec) {
        return util::unexpected{fmt::format("unable to stat {}: {}",
                                            squashfs.string(), ec.message())};
    }

    spdlog::debug("oci::push_squashfs {} ({} bytes) -> {}",
                  layer_digest->string(), size, ref.string());

    // upload the squashfs blob (streamed) and the empty config.
    if (auto ok =
            c.put_blob(layer_digest.value(), squashfs, std::move(progress));
        !ok) {
        return util::unexpected{ok.error().message};
    }
    if (auto ok = put_empty_config(c); !ok) {
        return util::unexpected{ok.error().message};
    }

    auto body = existing_manifest_body
                    ? std::move(*existing_manifest_body)
                    : serialize_manifest(make_squashfs_manifest(
                          layer_digest.value(), size, rfc3339_now()));
    if (auto ok = c.put_manifest(ref, body); !ok) {
        return util::unexpected{ok.error().message};
    }

    // the canonical id is the digest of the manifest bytes we just PUT.
    return digest_of_string(body);
}

util::expected<descriptor, std::string> attach(client& c,
                                               const reference& subject,
                                               std::string_view artifact_type,
                                               const fs::path& payload) {
    // stat the payload once, and branch file-vs-directory off that single
    // result (avoids a TOCTOU between exists() and is_directory()).
    std::error_code ec;
    auto payload_status = fs::status(payload, ec);
    if (ec || !fs::exists(payload_status)) {
        return util::unexpected{
            fmt::format("payload {} does not exist", payload.string())};
    }
    const bool payload_is_dir = fs::is_directory(payload_status);
    if (util::file_access_level(payload) < util::file_level::readonly) {
        return util::unexpected{
            fmt::format("payload {} is not readable", payload.string())};
    }

    // resolve the subject (the image we annotate).
    auto subj = c.get_manifest(subject);
    if (!subj) {
        return util::unexpected{fmt::format("unable to resolve subject {}: {}",
                                            subject.string(), subj.error())};
    }
    descriptor subject_desc{.media_type = subj->media_type.empty()
                                              ? std::string{media_type_manifest}
                                              : subj->media_type,
                            .digest = subj->digest,
                            .size = subj->body.size()};

    // package the payload.
    util::expected<packaged_layer, std::string> packaged =
        payload_is_dir ? package_directory(payload) : package_file(payload);
    if (!packaged) {
        return util::unexpected{packaged.error()};
    }
    auto cleanup = [&packaged] {
        if (packaged->scratch) {
            std::error_code ec;
            fs::remove_all(*packaged->scratch, ec);
        }
    };

    // upload the layer blob and empty config.
    if (auto ok = c.put_blob(packaged->layer.digest, packaged->blob); !ok) {
        cleanup();
        return util::unexpected{ok.error().message};
    }
    cleanup();
    if (auto ok = put_empty_config(c); !ok) {
        return util::unexpected{ok.error().message};
    }

    // build + push the referrer manifest.
    manifest m;
    m.artifact_type = std::string{artifact_type};
    m.annotations[std::string{annotation_created}] = rfc3339_now();
    m.subject = subject_desc;
    m.layers.push_back(packaged->layer);

    auto body = serialize_manifest(m);
    const auto manifest_digest = digest_of_string(body);
    if (auto ok = c.put_manifest(reference::digest(manifest_digest), body);
        !ok) {
        return util::unexpected{ok.error().message};
    }

    descriptor referrer{.media_type = std::string{media_type_manifest},
                        .digest = manifest_digest,
                        .size = body.size(),
                        .artifact_type = std::string{artifact_type}};

    // ensure the referrer is discoverable even on registries that do not
    // auto-index the Referrers API.
    maintain_referrers_tag(c, subject_desc.digest, referrer);

    return referrer;
}

util::expected<void, std::string>
copy_image(const util::url& registry_base, const std::string& src_repo,
           const std::string& dst_repo, const digest& src_manifest,
           const std::string& dst_tag, std::optional<credentials> creds) {
    auto src = client::create(registry_base, src_repo, creds);
    if (!src) {
        return util::unexpected{src.error().message};
    }
    auto dst = client::create(registry_base, dst_repo, creds);
    if (!dst) {
        return util::unexpected{dst.error().message};
    }
    auto dst_ref = oci::tag::parse(dst_tag);
    if (!dst_ref) {
        return util::unexpected{fmt::format("invalid destination tag '{}': {}",
                                            dst_tag,
                                            dst_ref.error().message())};
    }
    // dst tokens need pull scope on the source repo for cross-repo mounts.
    dst->add_pull_scope(src_repo);

    // fetch the image manifest and move its blobs.
    auto mr = src->get_manifest(reference::digest(src_manifest));
    if (!mr) {
        return util::unexpected{
            fmt::format("unable to fetch source manifest {}: {}",
                        src_manifest.string(), mr.error())};
    }
    const digest manifest_digest = mr->digest;
    const std::string_view manifest_media =
        mr->media_type.empty() ? media_type_manifest
                               : std::string_view{mr->media_type};

    auto parsed = parse_manifest(mr->body);
    if (!parsed) {
        return util::unexpected{parsed.error()};
    }
    for (const auto& d : blob_digests(*parsed)) {
        if (auto ok = copy_blob(*src, *dst, src_repo, d); !ok) {
            return util::unexpected{ok.error()};
        }
    }
    // PUT the image manifest under the destination tag. Content is
    // byte-for-byte identical, so the digest (identity) is preserved.
    if (auto ok = dst->put_manifest(reference::tag(*dst_ref), mr->body,
                                    manifest_media);
        !ok) {
        return util::unexpected{ok.error().message};
    }

    // copy referrers (the --recursive part): meta and any other attachments.
    // referrers() already degrades gracefully on registries without the
    // Referrers API (tag-schema fallback, empty list when nothing is
    // attached), so any error here is a real failure — surface it rather
    // than silently copying the image without its metadata.
    auto refs = src->referrers(manifest_digest);
    if (!refs) {
        return util::unexpected{
            fmt::format("unable to list the artifacts attached to {}: {}",
                        manifest_digest.string(), refs.error().message)};
    }
    for (const auto& r : *refs) {
        auto rm = src->get_manifest(reference::digest(r.digest));
        if (!rm) {
            return util::unexpected{
                fmt::format("unable to fetch referrer {}: {}",
                            r.digest.string(), rm.error())};
        }
        auto rparsed = parse_manifest(rm->body);
        if (!rparsed) {
            return util::unexpected{rparsed.error()};
        }
        for (const auto& d : blob_digests(*rparsed)) {
            if (auto ok = copy_blob(*src, *dst, src_repo, d); !ok) {
                return util::unexpected{ok.error()};
            }
        }
        const std::string_view rmedia = rm->media_type.empty()
                                            ? media_type_manifest
                                            : std::string_view{rm->media_type};
        // the referrer's subject digest is the image manifest digest, which is
        // unchanged by the copy, so the manifest is valid verbatim in dst.
        if (auto ok = dst->put_manifest(reference::digest(r.digest), rm->body,
                                        rmedia);
            !ok) {
            return util::unexpected{ok.error().message};
        }
        // referrer manifests are pushed by digest only, which registries
        // without the Referrers API do not index: maintain the tag-schema
        // index on the destination so the attachment stays discoverable
        // there too.
        maintain_referrers_tag(*dst, manifest_digest, r);
    }

    return {};
}

} // namespace oci
