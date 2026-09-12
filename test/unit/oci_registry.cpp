// Live registry round-trip tests for the native OCI client.
//
// These drive push/pull/attach/referrers/copy against a real registry. A zot
// binary is started lazily on the first [registry] case and killed at process
// exit; each test uses a unique repository path, so OCI's per-repository
// namespacing keeps them isolated. If no zot binary is found the cases SKIP, so
// a normal `./test/unit` run is unaffected.
//
// The binary is located via -DUENV_TEST_ZOT_PATH (set by meson), the
// UENV_TEST_ZOT env override, or the PATH.

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <catch2/catch_all.hpp>
#include <fmt/format.h>

#include <oci/client.h>
#include <oci/digest.h>
#include <oci/manifest.h>
#include <oci/pull.h>
#include <oci/push.h>
#include <oci/reference.h>
#include <oci/util.h>
#include <util/curl.h>
#include <util/fs.h>
#include <util/sha.h>
#include <util/subprocess.h>
#include <util/url.h>

namespace {

// locate the zot binary: build-time path, env override, then PATH.
std::string zot_binary() {
    if (const char* e = std::getenv("UENV_TEST_ZOT")) {
        if (::access(e, X_OK) == 0) {
            return e;
        }
    }
#ifdef UENV_TEST_ZOT_PATH
    if (::access(UENV_TEST_ZOT_PATH, X_OK) == 0) {
        return UENV_TEST_ZOT_PATH;
    }
#endif
    // let execvp resolve "zot" on PATH; empty means "not found here".
    return "zot";
}

int free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool ready(const std::string& base) {
    util::curl::request req;
    req.url = base + "/v2/";
    auto resp = util::curl::perform(req);
    return resp && (resp->status == 200 || resp->status == 401);
}

// a lazily-started zot registry, shared by all [registry] cases and killed when
// the process exits.
struct zot_instance {
    std::optional<util::subprocess> proc;
    std::string base; // http://127.0.0.1:PORT, empty when unavailable
    std::filesystem::path state;

    zot_instance() {
        const auto zot = zot_binary();
        const int port = free_port();
        state = util::make_temp_dir().value();
        const auto cfg = state / "config.json";
        {
            std::ofstream f{cfg};
            f << fmt::format(R"({{"storage":{{"rootDirectory":"{}"}},)"
                             R"("http":{{"address":"127.0.0.1","port":"{}"}},)"
                             R"("log":{{"level":"error","output":"{}"}}}})",
                             (state / "registry").string(), port,
                             (state / "zot.log").string());
        }
        auto p = util::run({zot, "serve", cfg.string()});
        if (!p) {
            return; // no zot binary / failed to launch -> tests skip
        }
        const auto url = fmt::format("http://127.0.0.1:{}", port);
        for (int i = 0; i < 150; ++i) { // up to ~30s
            if (p->finished()) {
                return; // zot died on startup
            }
            if (ready(url)) {
                proc = std::move(*p);
                base = url;
                return;
            }
            ::usleep(200 * 1000);
        }
        p->kill();
    }

    ~zot_instance() {
        if (proc) {
            proc->kill();
        }
    }
};

// the shared registry base URL, or empty when no registry could be started.
const std::string& registry_base() {
    static zot_instance instance;
    return instance.base;
}

// the same base as a url. Only valid once registry_base() is known non-empty,
// i.e. after the SKIP check.
util::url registry_url() {
    return *util::parse_url(registry_base());
}

// A lying registry: a plain static file server whose directory tree mimics the
// distribution API, so a manifest body can be served at a path naming a digest
// the body does not hash to. A content-addressed registry such as zot cannot
// produce that response, which is the point — this stands in for a compromised
// or malicious registry. Needs only python3; the cases SKIP without it.
struct static_registry {
    std::optional<util::subprocess> proc;
    std::string base; // http://127.0.0.1:PORT, empty when unavailable
    std::filesystem::path root;

    static_registry() {
        root = util::make_temp_dir().value();
        // GET /v2/ must answer 200 for the client to bind anonymously; a
        // directory listing does.
        std::filesystem::create_directories(root / "v2");
        const int port = free_port();
        auto p =
            util::run({"python3", "-m", "http.server", std::to_string(port),
                       "--bind", "127.0.0.1", "--directory", root.string()});
        if (!p) {
            return; // no python3 -> tests skip
        }
        const auto url = fmt::format("http://127.0.0.1:{}", port);
        for (int i = 0; i < 100; ++i) { // up to ~20s
            if (p->finished()) {
                return;
            }
            if (ready(url)) {
                proc = std::move(*p);
                base = url;
                return;
            }
            ::usleep(200 * 1000);
        }
        p->kill();
    }

    ~static_registry() {
        if (proc) {
            proc->kill();
        }
    }
};

// the shared static server, or empty base when one could not be started.
const static_registry& lying_registry() {
    static static_registry instance;
    return instance;
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream f{path, std::ios::binary};
    f << content;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream f{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("oci registry push/pull round-trip", "[registry]") {
    const auto base = registry_base();
    if (base.empty()) {
        SKIP("no zot binary available for the registry tests");
    }

    auto dir = util::make_temp_dir().value();
    const auto sqfs = dir / "store.squashfs";
    const std::string payload = "squashfs-bytes-round-trip";
    write_file(sqfs, payload);

    auto c = oci::client::create(registry_url(), "test/app/1.0");
    REQUIRE(c.has_value());

    auto pushed = oci::push_squashfs(
        *c, sqfs, oci::reference::tag(oci::tag::parse("v1").value()));
    REQUIRE(pushed.has_value());

    // the pushed digest is the sha256 of the manifest the registry stores.
    auto resp =
        c->get_manifest(oci::reference::tag(oci::tag::parse("v1").value()));
    REQUIRE(resp.has_value());
    const auto local = oci::digest::sha256(util::sha256_string(resp->body));
    REQUIRE(local == *pushed);
    // get_manifest computes the digest itself rather than reading the
    // registry's Docker-Content-Digest header.
    REQUIRE(resp->digest == *pushed);

    // fetching by that digest is a pin: the same bytes come back, and the
    // response reports the digest that was asked for.
    auto by_digest = c->get_manifest(oci::reference::digest(*pushed));
    REQUIRE(by_digest.has_value());
    REQUIRE(by_digest->body == resp->body);
    REQUIRE(by_digest->digest == *pushed);

    // pull the layer back; pull_squashfs self-verifies its digest internally.
    auto image = oci::parse_manifest(resp->body);
    REQUIRE(image.has_value());
    auto store = util::make_temp_dir().value();
    auto pulled = oci::pull_squashfs(*c, *image, store);
    REQUIRE(pulled.has_value());
    REQUIRE(read_file(store / "store.squashfs") == payload);
}

TEST_CASE("oci registry failed blob download leaves no file", "[registry]") {
    const auto base = registry_base();
    if (base.empty()) {
        SKIP("no zot binary available for the registry tests");
    }

    auto c = oci::client::create(registry_url(), "test/app/1.0");
    REQUIRE(c.has_value());

    // a valid digest that no blob in the registry has: the download must
    // fail, and must not leave anything at the destination path (or a
    // .partial next to it) that a later pull would mistake for a complete
    // blob.
    const auto missing =
        oci::digest::sha256(util::sha256_string("no-such-blob"));
    auto store = util::make_temp_dir().value();
    const auto dest = store / "store.squashfs";

    auto pulled = c->get_blob_to_file(missing, dest);
    REQUIRE(!pulled.has_value());
    REQUIRE(!std::filesystem::exists(dest));
    REQUIRE(!std::filesystem::exists(store / "store.squashfs.partial"));
}

TEST_CASE("oci registry attach + referrers + pull_meta", "[registry]") {
    const auto base = registry_base();
    if (base.empty()) {
        SKIP("no zot binary available for the registry tests");
    }

    auto dir = util::make_temp_dir().value();
    const auto sqfs = dir / "store.squashfs";
    write_file(sqfs, "sqfs");
    const auto meta = dir / "meta";
    std::filesystem::create_directories(meta);
    write_file(meta / "env.json", R"({"views":{}})");

    auto c = oci::client::create(registry_url(), "test/meta-app/1.0");
    REQUIRE(c.has_value());

    auto pushed = oci::push_squashfs(
        *c, sqfs, oci::reference::tag(oci::tag::parse("v1").value()));
    REQUIRE(pushed.has_value());

    auto att =
        oci::attach(*c, oci::reference::digest(*pushed), "uenv/meta", meta);
    REQUIRE(att.has_value());

    auto refs = c->referrers(*pushed);
    REQUIRE(refs.has_value());
    bool found_meta = false;
    for (const auto& d : *refs) {
        if (d.artifact_type && *d.artifact_type == "uenv/meta") {
            found_meta = true;
        }
    }
    REQUIRE(found_meta);

    // the referrers tag schema (<algo>-<hex>) is the fallback used by
    // client::referrers on registries without the Referrers API; attach must
    // maintain it, and its index must list the same referrers as the API.
    const auto fallback_tag = oci::reference::tag(
        oci::tag::parse(pushed->algorithm() + "-" + pushed->hex()).value());
    auto tag_index = c->get_manifest(fallback_tag);
    REQUIRE(tag_index.has_value());
    auto tag_refs = oci::detail::parse_referrers(tag_index->body);
    REQUIRE(tag_refs.has_value());
    REQUIRE(*tag_refs == *refs);

    // attaching a second artifact must merge into the tag index, not clobber
    // the entry the first attach wrote.
    const auto extra = dir / "notes.json";
    write_file(extra, R"({"note":"x"})");
    auto att2 =
        oci::attach(*c, oci::reference::digest(*pushed), "uenv/extra", extra);
    REQUIRE(att2.has_value());

    auto tag_index2 = c->get_manifest(fallback_tag);
    REQUIRE(tag_index2.has_value());
    auto tag_refs2 = oci::detail::parse_referrers(tag_index2->body);
    REQUIRE(tag_refs2.has_value());
    REQUIRE(tag_refs2->size() == 2);
    bool tag_has_meta = false;
    bool tag_has_extra = false;
    for (const auto& d : *tag_refs2) {
        tag_has_meta = tag_has_meta || d.artifact_type == "uenv/meta";
        tag_has_extra = tag_has_extra || d.artifact_type == "uenv/extra";
    }
    REQUIRE(tag_has_meta);
    REQUIRE(tag_has_extra);

    auto store = util::make_temp_dir().value();
    auto got = oci::pull_meta(*c, *pushed, store);
    REQUIRE(got.has_value());
    REQUIRE(*got);
    REQUIRE(std::filesystem::exists(store / "meta" / "env.json"));
    REQUIRE(read_file(store / "meta" / "env.json") == R"({"views":{}})");
}

TEST_CASE("oci registry copy preserves digest", "[registry]") {
    const auto base = registry_base();
    if (base.empty()) {
        SKIP("no zot binary available for the registry tests");
    }

    auto dir = util::make_temp_dir().value();
    const auto sqfs = dir / "store.squashfs";
    write_file(sqfs, "copy-me");

    const std::string src_repo = "test/copy-src/1.0";
    const std::string dst_repo = "test/copy-dst/1.0";

    auto src = oci::client::create(registry_url(), src_repo);
    REQUIRE(src.has_value());
    auto pushed = oci::push_squashfs(
        *src, sqfs, oci::reference::tag(oci::tag::parse("v1").value()));
    REQUIRE(pushed.has_value());

    // attach metadata to the source image: copy must carry it across.
    const auto meta = dir / "meta";
    std::filesystem::create_directories(meta);
    write_file(meta / "env.json", R"({"views":{}})");
    auto att =
        oci::attach(*src, oci::reference::digest(*pushed), "uenv/meta", meta);
    REQUIRE(att.has_value());

    auto copied = oci::copy_image(registry_url(), src_repo, dst_repo, *pushed,
                                  "v1", std::nullopt);
    REQUIRE(copied.has_value());

    // the destination manifest is byte-identical, so its digest is preserved.
    auto dst = oci::client::create(registry_url(), dst_repo);
    REQUIRE(dst.has_value());
    auto resp =
        dst->get_manifest(oci::reference::tag(oci::tag::parse("v1").value()));
    REQUIRE(resp.has_value());
    const auto local = oci::digest::sha256(util::sha256_string(resp->body));
    REQUIRE(local == *pushed);

    // the attached metadata is discoverable and pullable from the
    // destination.
    auto store = util::make_temp_dir().value();
    auto got = oci::pull_meta(*dst, *pushed, store);
    REQUIRE(got.has_value());
    REQUIRE(*got);
    REQUIRE(read_file(store / "meta" / "env.json") == R"({"views":{}})");

    // copy must also recreate the referrers tag-schema index on the
    // destination, so the attachment is discoverable on registries without
    // the Referrers API.
    const auto fallback_tag = oci::reference::tag(
        oci::tag::parse(pushed->algorithm() + "-" + pushed->hex()).value());
    auto tag_index = dst->get_manifest(fallback_tag);
    REQUIRE(tag_index.has_value());
    auto tag_refs = oci::detail::parse_referrers(tag_index->body);
    REQUIRE(tag_refs.has_value());
    REQUIRE(tag_refs->size() == 1);
    REQUIRE(tag_refs->front().artifact_type == "uenv/meta");
}

TEST_CASE("oci registry push_squashfs with a precomputed digest",
          "[registry]") {
    const auto base = registry_base();
    if (base.empty()) {
        SKIP("no zot binary available for the registry tests");
    }

    auto dir = util::make_temp_dir().value();
    const auto sqfs = dir / "store.squashfs";
    const std::string payload = "squashfs-bytes-precomputed-digest";
    write_file(sqfs, payload);

    auto c = oci::client::create(registry_url(), "test/precomputed/1.0");
    REQUIRE(c.has_value());

    // supplying the layer digest spares push_squashfs a full read of the file.
    const auto layer = oci::digest::sha256(util::sha256_string(payload));
    auto pushed = oci::push_squashfs(
        *c, sqfs, oci::reference::tag(oci::tag::parse("v1").value()), layer);
    REQUIRE(pushed.has_value());

    // the manifest records the digest we supplied, and the layer is addressable
    // under it. (The manifest digest itself is not compared against a
    // hash-it-yourself push: push_squashfs stamps a wall-clock `created`
    // annotation, so two pushes need not produce identical manifests.)
    auto resp =
        c->get_manifest(oci::reference::tag(oci::tag::parse("v1").value()));
    REQUIRE(resp.has_value());
    auto image = oci::parse_manifest(resp->body);
    REQUIRE(image.has_value());
    REQUIRE(image->layers.size() == 1);
    REQUIRE(image->layers[0].digest == layer);

    const auto blob = dir / "pulled.squashfs";
    REQUIRE(c->get_blob_to_file(layer, blob).has_value());
    REQUIRE(read_file(blob) == payload);
}

// The registry is the one thing a pull cannot take on trust: every layer
// digest that get_blob_to_file verifies against is read out of the manifest,
// so a manifest accepted without checking makes every check below it
// self-referential.
TEST_CASE("oci registry a manifest is rejected unless it hashes to the "
          "requested digest",
          "[registry]") {
    const auto& reg = lying_registry();
    if (reg.base.empty()) {
        SKIP("no python3 available to serve the static registry");
    }

    const std::string repo = "test/lying/1.0";
    const std::string body = R"({"schemaVersion":2,"layers":[]})";
    const auto dir =
        reg.root / "v2" / std::filesystem::path{repo} / "manifests";
    REQUIRE(util::ensure_directory(dir).has_value());

    auto c = oci::client::create(*util::parse_url(reg.base), repo);
    REQUIRE(c.has_value());

    // the caller pins a digest; the registry answers with different bytes.
    const auto pinned =
        oci::digest::sha256(util::sha256_string("the manifest that was asked "
                                                "for"));
    write_file(dir / pinned.string(), body);
    auto swapped = c->get_manifest(oci::reference::digest(pinned));
    REQUIRE(!swapped.has_value());
    REQUIRE(swapped.error().message.find("digest mismatch") !=
            std::string::npos);

    // the same bytes under their own digest are accepted, and the response
    // carries the locally computed digest.
    const auto honest = oci::digest::sha256(util::sha256_string(body));
    write_file(dir / honest.string(), body);
    auto ok = c->get_manifest(oci::reference::digest(honest));
    REQUIRE(ok.has_value());
    REQUIRE(ok->body == body);
    REQUIRE(ok->digest == honest);

    // a digest the client cannot compute is a hard error, not an unverified
    // pass-through.
    const auto sha512 = oci::digest::parse("sha512:" + std::string(128, 'a'));
    REQUIRE(sha512.has_value());
    write_file(dir / sha512->string(), body);
    auto unsupported = c->get_manifest(oci::reference::digest(*sha512));
    REQUIRE(!unsupported.has_value());
    REQUIRE(unsupported.error().message.find("unsupported digest algorithm") !=
            std::string::npos);
}
