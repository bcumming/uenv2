# `src/oci` — a native OCI registry client

This module talks to an OCI container registry over HTTP: it pulls, pushes,
copies and inspects the artifacts that uenv stores there. It is the native
replacement for the external `oras` binary that uenv used to shell out to, and
it produces artifacts that are byte-for-byte compatible with what `oras` wrote,
so images pushed by either tool are readable by the other.

The module is deliberately self-contained. It depends only on `src/util/` and on
external libraries (fmt, nlohmann_json, libcurl, spdlog, zlib), and it must
**not** include `src/uenv/`, `src/site/` or `src/cli/`. Anything both `uenv` and
`oci` need — the lexer, the parsing scaffolding, the sha helpers, the url type —
lives in `src/util/` for exactly this reason. The rule is enforced by a grep that
must return nothing:

```bash
grep -rn '#include <\(uenv\|site\|cli\)/' src/oci/
```

Everything lives in namespace `oci`. Fallible operations return
`util::expected<T, E>` rather than throwing.

---

## Glossary

Registries speak a vocabulary of near-synonyms that are easy to confuse. This
module gives most of these terms a distinct C++ type precisely so that they
cannot be mixed up, so it is worth pinning down what each one means.

**Registry** — the server that stores artifacts, e.g.
`https://jfrog.svc.cscs.ch`. It speaks the OCI distribution API under the `/v2/`
path. In uenv's config this is the `registry.url` setting, which may include a
path prefix (`jfrog.svc.cscs.ch/uenv`); `split_registry` separates the two parts.

**Repository** — a named collection of artifacts on one registry, e.g.
`uenv/deploy/todi/gh200/prgenv-gnu/24.11`. uenv derives the repository path from
the uenv label with `repository_path`: `<prefix>/<namespace>/<system>/<uarch>/
<name>/<version>`. Everything in the distribution API is scoped to a repository:
blobs, manifests, tags and auth tokens all live under one.

**Blob** — an opaque byte payload stored in a repository and addressed only by
its digest. The squashfs image is a blob; so is the gzipped tar of the metadata,
and so is the (tiny) config object. Blobs are content-addressed, which means a
registry can dedupe them and can serve a blob from backing storage via a
redirect.

**Digest** — the content address of a blob or manifest: an algorithm plus a
lowercase-hex hash of the bytes, written `sha256:abc123…`. A digest is
*immutable* and *self-verifying*: fetch the bytes, hash them, and you can prove
you got the right thing. Represented by `oci::digest`.

**Tag** — a mutable, human-readable name for a manifest within a repository,
e.g. `v1`. A tag can be moved to point at different content tomorrow; a digest
cannot. Represented by `oci::tag`, which validates the OCI tag grammar
(`[a-zA-Z0-9_][a-zA-Z0-9._-]{0,127}`).

**Reference** — whatever goes in the `/manifests/<reference>` slot of a registry
URL: *either* a tag *or* a digest. The two are interchangeable positionally but
mean quite different things, so `oci::reference` keeps them distinct at the type
level while allowing either to be passed where a manifest is addressed.

**Descriptor** — a small record that points at content: a media type, a digest,
a size, and optionally an artifact type. Descriptors are how a manifest names
its layers, how a referrer names its subject, and how the referrers list names
each attachment. Represented by `oci::descriptor`.

**Manifest** — the JSON document that *is* the image: it lists the layer
descriptors, the config descriptor, an optional subject, and annotations. A
manifest is itself content-addressed, and the digest of its bytes is the image's
canonical identity — this is the sha uenv stores in its repository database and
displays as the image id.

**Layer** — one entry in a manifest's `layers` array: a blob descriptor plus
annotations. uenv images carry a single layer, the squashfs, annotated with the
title `store.squashfs`.

**Artifact type** — a manifest-level label saying what kind of thing this is,
analogous to a file extension. uenv uses `application/x-squashfs` for images and
`uenv/meta` for attached metadata.

**Referrer / attachment** — a manifest whose `subject` field points at another
manifest, i.e. side-data attached to an image. uenv attaches its `meta/`
directory to an image this way. Listing the referrers of an image is how you
discover its metadata (the equivalent of `oras discover`).

**Referrers tag schema** — the fallback for registries that do not implement the
OCI 1.1 Referrers API. Instead of asking the registry to index attachments, the
pushing client maintains an index manifest under the tag `<algo>-<hex>` naming
the subject. `client::referrers` reads the API first and falls back to this tag;
the push side maintains it. Callers never see the difference.

**Bearer challenge** — the `WWW-Authenticate` header a registry returns with a
401, naming a token endpoint (`realm`), a `service`, and the `scopes` at issue.
The client answers it by fetching a token and retrying.

**Scope** — the permission a token carries, written
`repository:<repo>:<actions>`, e.g. `repository:uenv/deploy/…:pull,push`. A
token is scoped to specific repositories and actions, which is why copying
between two repositories needs a token carrying scopes for both.

**Empty config** — OCI requires every manifest to have a config descriptor, but
an artifact like a squashfs image has no meaningful config. The convention
(followed by oras) is a canonical descriptor for the two-byte body `{}`, which
this module exposes as `empty_config_descriptor()`.

---

## API

### `digest.h` — content addresses

```cpp
class digest {
    static digest sha256(const util::sha256& hex);
    static util::expected<digest, util::parse_error> parse(std::string_view);
    const std::string& algorithm() const;
    const std::string& hex() const;
    std::string string() const;   // "sha256:abc…"
};
```

A `digest` is always syntactically valid — there is no public constructor, so the
only ways to get one are parsing (which validates the algorithm and the hex) or
promoting an already-validated hash via `digest::sha256`. That means callers
never have to wonder whether a given string is bare hex or prefixed, or whether
it might actually be a tag. Comparison and `fmt` formatting are supported.

`digest::sha256` is the bridge from the rest of uenv: a `uenv::record`'s `sha` is
a `util::sha256`, and `oci::digest::sha256(record.sha)` turns it into the
manifest digest that identifies the image in the registry.

### `tag.h` — mutable names

```cpp
class tag {
    static util::expected<tag, util::parse_error> parse(std::string_view);
    const std::string& string() const;
};
```

Same discipline as `digest`: only obtainable by parsing, so a `tag` value always
satisfies the OCI tag grammar. Rejects an empty value, a leading `.` or `-`, an
out-of-grammar character, and anything over 128 characters.

### `reference.h` — "tag or digest"

```cpp
class reference {
    static reference tag(oci::tag);
    static reference digest(oci::digest);
    static util::expected<reference, util::parse_error> parse(std::string_view);
    bool is_tag() const;  bool is_digest() const;
    const oci::tag& as_tag() const;  const oci::digest& as_digest() const;
    std::string string() const;
};
```

`reference::parse` resolves the ambiguity by trying the digest grammar first:
text that parses as `<algo>:<hex>` becomes a digest reference, anything else is
validated as a tag. Client operations that address a manifest
(`get_manifest`, `put_manifest`, `attach`'s subject) take a `reference`, so a
caller can supply whichever it holds.

### URLs — `util::url`

Every url in this module is a `util::url` (`src/util/url.h`), not a string. It
lives in `src/util` rather than here because uenv's configuration holds urls too,
and `src/oci` must never be something `src/uenv` depends on.

Like `digest` and `tag`, it is parse-don't-validate: the only way to get one is
`util::parse_url`, so a `url` is always well-formed. It canonicalises on parse —
scheme and host are lowercased, which RFC 3986 permits because both are
case-insensitive — but touches nothing else: percent-encoded octets pass through
verbatim and the path keeps its case, because repository names and digests travel
there and must survive byte-exact.

Three operations carry the weight:

| operation | replaces |
| --- | --- |
| `origin()` — `scheme://host[:port]`, no path, no trailing `/` | a trailing-slash strip hand-rolled at three call sites |
| `resolve(path)` — join with exactly one `/` | `base + path` concatenation |
| `query_param(k, v)` — encode, and pick `?` or `&` | the separator dance open-coded at each query builder |

`query_param`'s encoder keeps the unreserved set plus `:` `/` `,` — the exact
alphabet of every value uenv sends (`repository:<repo>:pull,push`,
`sha256:<hex>`, `a/b/c`), so it is a no-op on them and changes no bytes on the
wire. It escapes `&`, `=`, `#`, `+`, space and control characters: what a
registry-supplied `service` or `scope` would otherwise use to inject query
parameters of its own.

**Where urls are parsed.** At the boundaries they enter:

- *configuration* — `registry.url`, `artifactory_url` and `listing_url` are
  parsed in `uenv::parse_config_toml`, so a malformed url fails at startup with
  the offending key named, rather than half way through a push. Each is
  guaranteed http or https, with https assumed when the config omits a scheme
  (the form the CSCS deployment uses).
- *response headers* — a Bearer challenge `realm` is parsed by
  `parse_bearer_challenge`; an upload `Location` by `detail::resolve_upload_url`.

**Transport policy.** `util::curl` already refuses to let a *redirect* downgrade
the transport or carry credentials across hosts. A `realm` and an absolute
`Location` are fresh requests rather than redirects, so they never reach that
guard — and both are about to be handed credentials (the realm gets the user's
password; the Location gets the push token and the blob body).
`detail::check_transport` applies the same policy to them by hand: the target
must speak http or https, and may not be weaker than the registry's own scheme.
A different *host* is deliberately allowed — Docker Hub challenges
`registry-1.docker.io` with `realm=auth.docker.io`, and uploads legitimately
redirect to blob storage.

### `types.h` — the connection-independent vocabulary

The OCI types that mean something without a registry connection: the media-type
constants, `descriptor`, the registry addressing helpers, and the raw result of a
manifest fetch. They live apart from `client.h` so that a consumer which only
speaks about OCI *content* — `manifest.h`, the response parsers in `util.h` —
does not have to see the client, and through it libcurl.

```cpp
struct descriptor {
    std::string media_type;
    oci::digest digest;
    std::size_t size = 0;
    std::optional<std::string> artifact_type;
    std::optional<std::string> data;   // inline base64, on the empty config
};

struct registry_location { util::url base; std::string prefix; };

registry_location split_registry(const util::url& configured_url);

std::string repository_path(prefix, nspace, system, uarch, name, version);

struct manifest_response {
    std::string body;
    oci::digest digest;                  // computed over body, never a header
    std::string media_type;
};
```

`split_registry` turns a configured `registry.url` into `{base, prefix}` — its
origin (`scheme://host[:port]`) and the repository prefix taken from its path.
It cannot fail: the url was already parsed when the configuration was read (see
"URLs" below). `repository_path` then builds the repository name from a uenv
label, matching the address `oras` used. Both are pure functions.

`manifest_response` keeps the raw bytes alongside their digest, which
`get_manifest` computes over those bytes. The `Docker-Content-Digest` header is
never used for it: the header is the registry's assertion about its own
response, so trusting it would let a registry name content it did not send.

### `client.h` — the registry connection

The central type. A `client` is bound to **one repository on one registry** and
handles authentication, retries and the HTTP mechanics of the distribution API.
It is **move-only**: obtain one from `create` and pass it around by reference
(every function in this module takes `client&`). Its implementation — the curl
requests and the token cache — sits behind a pimpl, so this header stays free of
`<curl/curl.h>`.

```cpp
static util::expected<client, client_error>
client::create(std::string registry_url, std::string repository,
               std::optional<credentials> creds = std::nullopt);
```

`create` probes the registry, parses its auth challenge, and binds to the
repository. Supply credentials for push or private pull; omit them for anonymous
pull. Authentication is **lazy**: no token is fetched until an operation needs
one, and the client caches a pull token and a pull,push token separately, so an
anonymous-capable read never triggers a credentialed handshake. Tokens are
refreshed when they approach their advertised expiry, and a 401 on a cached token
(common when a token quietly lapses during a long multi-GB transfer) triggers one
transparent refresh-and-retry.

Read operations:

| method | purpose |
| --- | --- |
| `blob_exists(digest)` | HEAD a blob; 200 → true, 404 → false |
| `get_blob_to_file(digest, path, progress, should_abort)` | stream a blob to disk, following redirects to backing storage |
| `get_manifest(reference)` | fetch a manifest by tag or digest, verifying the bytes against the digest |
| `list_tags()` | list the repository's tags |
| `referrers(digest)` | list artifacts attached to a manifest |

Write operations:

| method | purpose |
| --- | --- |
| `put_blob(digest, path, progress)` | upload a blob, streamed from disk |
| `put_blob_bytes(digest, data)` | upload a small in-memory blob |
| `put_manifest(reference, body, media_type)` | PUT a manifest under a tag or digest |
| `mount_blob(digest, from_repository)` | cross-repo mount; true if mounted, false if the registry wants a full upload |
| `add_pull_scope(repository)` | add a repository to this client's token scopes |

`get_blob_to_file` and `put_blob` never hold the payload in memory, which is what
makes multi-GB squashfs images workable. Both take an optional progress callback
receiving `(bytes_so_far, bytes_total)`; `get_blob_to_file` also takes an abort
predicate, polled during the transfer, for Ctrl-C handling. `put_blob` and
`put_blob_bytes` are no-ops when the registry already has the blob.

Errors are `client_error`:

```cpp
struct client_error {
    std::string message;
    std::optional<long> http_status = std::nullopt;
};
```

`http_status` is set when an HTTP response was received and `nullopt` otherwise —
transport failures (DNS, connect, TLS, dropped connection), token-fetch failures
and local errors (unwritable destination, digest mismatch). Callers that must
distinguish "not found" (often an expected outcome) from a transient failure
branch on `http_status`; everyone else just prints `message`, for which a `fmt`
formatter is provided.

### `manifest.h` — the image document

`manifest` and `manifest_layer` model an OCI image manifest, and are used for
both reading and writing:

```cpp
std::string serialize_manifest(const manifest&);
util::expected<manifest, std::string> parse_manifest(std::string_view body);
std::string serialize_index(const std::vector<descriptor>&);
```

`serialize_manifest` emits the same byte shape oras produces (annotations live in
a sorted `std::map` so serialization is deterministic and matches Go's
sorted-key map output) — which matters, because the manifest bytes *are* the
image identity. `serialize_index` writes an image index, used for the referrers
tag fallback.

Two lookup helpers save callers from open-coding annotation searches:
`manifest::find_layer_by_title(title)` and `manifest::find_unpack_layer()`.

The header also carries the constants: media types, artifact types
(`artifact_type_squashfs`, `artifact_type_meta`), annotation keys, and
`empty_config_descriptor()`.

### `auth.h` — credentials and tokens

```cpp
struct credentials { std::string username, password; };  // password or PAT
struct bearer_challenge { util::url realm; std::string service; std::vector<std::string> scopes; };
struct token_response { std::string token; std::optional<long> expires_in; };
```

`credentials` has a `fmt` formatter that redacts the password, so it is safe to
log.

Every field of a `bearer_challenge` is supplied by the registry, so it is an
untrusted-input boundary: `realm` is parsed into a `util::url` as the challenge
is parsed, and `service`/`scopes` are percent-encoded when they are built into
the token url. The realm must additionally clear `detail::check_transport`
before it is fetched — see "URLs" below.

Network operations — `discover_challenge` and `fetch_token` — are used
internally by `client` and are exposed mainly for testing; ordinary callers
never need them.

Credential *resolution* is the part callers do use:

```cpp
struct credential_sources {
    std::optional<std::filesystem::path> explicit_token;   // --token
    std::optional<std::string> username;                   // username for the token sources
    std::optional<std::filesystem::path> uenv_token_dir;   // $XDG_CONFIG_HOME/uenv/tokens
    std::optional<std::filesystem::path> docker_config;    // ~/.docker/config.json
};

util::expected<std::optional<credentials>, std::string>
resolve_credentials(std::string_view registry_host, const credential_sources&);
```

`resolve_credentials` tries, in order: the explicit `--token` path, the uenv
token store (`<uenv_token_dir>/<registry_host>`), then the docker
`config.json` — including its credential helpers, where a per-registry
`credHelpers` entry beats the global `credsStore`, as in docker itself. It
returns `std::nullopt` when nothing is found, which means anonymous access, and
an error only when a source was present but unusable.

Note the split of responsibility: `credential_sources` is populated by the
*caller*, because knowing where uenv keeps its tokens is uenv's business, not the
registry client's. In the CLI that assembly lives in
`uenv::resolve_registry_credentials` (`src/cli/util.h`). This is what keeps
`src/oci` free of any dependency on `src/uenv`.

The split extends to the username. `resolve_credentials` never guesses one:
sources 1 and 2 fail with `oci::username_required_error` when `username` is
unset, and the caller — which knows what its own flags are called, and where it
keeps its environment — turns that into actionable advice. Source 3 carries its
own username and ignores the field.

### `pull.h` — the read workflows

```cpp
util::expected<void, std::string>
pull_squashfs(client&, const manifest& image, const std::filesystem::path& store,
              progress_fn progress = {}, std::function<bool()> should_abort = {});

util::expected<bool, std::string>
pull_meta(client&, const digest& manifest_digest,
          const std::filesystem::path& store);
```

`pull_squashfs` picks the layer titled `store.squashfs` (falling back to the sole
layer), downloads it to `<store>/store.squashfs`, and verifies it: the file is
hashed as it streams and checked against the layer digest, so a truncated or
corrupted download fails locally rather than producing a broken image.

`pull_meta` finds the `uenv/meta` referrer, downloads its gzipped tar and
unpacks it into `<store>`, reproducing the `meta/` directory. It returns `false`
(not an error) when the image simply has no attached metadata, which lets the
caller decide whether that is fatal. The archive is staged in a private temp
directory and digest-verified before it is unpacked (in-process via
`util::extract_targz`, which also rejects any entry that would escape the
destination) — the extracted `env.json` and views are later sourced into user
environments, so they are not taken on trust.

### `push.h` — the write workflows

```cpp
util::expected<digest, std::string>
push_squashfs(client&, const std::filesystem::path& squashfs, const reference&,
              std::optional<digest> layer_digest = std::nullopt,
              progress_fn progress = {});

util::expected<descriptor, std::string>
attach(client&, const reference& subject, std::string_view artifact_type,
       const std::filesystem::path& payload);

util::expected<void, std::string>
copy_image(const std::string& registry_base, const std::string& src_repo,
           const std::string& dst_repo, const digest& src_manifest,
           const std::string& dst_tag, std::optional<credentials> creds);
```

`push_squashfs` replaces `oras push --artifact-type application/x-squashfs`: it
uploads the blob and the empty config, builds the manifest with the layer titled
`store.squashfs`, PUTs it under `ref`, and returns the **manifest digest** — the
canonical image id, which is what the caller records in the uenv database. Pass
`layer_digest` when the sha is already known; otherwise `push_squashfs` reads the
whole file to compute it, which for a multi-GB image is minutes of work done
twice.

`attach` replaces `oras attach`. A directory payload is packed as a
deterministic gzipped tar in-process via `util::pack_directory_targz` (sorted
names, zeroed mtimes and ownership, `gzip -n`, and — crucially for oras
compatibility — no record padding past the tar EOF marker), so the layer digest
is stable across runs — this is what makes re-pushing the same metadata a no-op
rather than churning blobs. It also maintains the
referrers tag index, best-effort, so attachments stay discoverable on registries
without the Referrers API.

`copy_image` replaces `oras cp --recursive`. It moves blobs by cross-repo mount
where the registry allows it and streams them through local disk where it does
not, and it copies referrers along with the image. Manifests are copied
byte-for-byte, so the image digest — its identity — survives the copy. It is the
one entry point that creates its own clients (two of them, source and
destination), because it inherently spans two repositories.

### `parse.h` — string parsers

Lexer-based parsers for the character-level string types, following the same
idiom as `src/uenv/parse.*` and sharing the `util::parse_error` / `PARSE` /
`util::parse_string` scaffolding from `src/util/parse.h`:

- `parse_digest`, `parse_tag`, `parse_reference` — the primitives behind the
  `parse` static members of `digest`, `tag` and `reference`.
- `parse_url` → `struct url` with the RFC-3986 components. Accepts a bare
  `host/prefix` with no scheme, as registry configs are written.
- `parse_bearer_challenge` — parses a `WWW-Authenticate: Bearer …` header.
- `parse_scopes` — splits a space-separated scope list; never fails.

JSON documents are parsed with nlohmann_json, not with these; the parsers here
handle string types only.

### `util.h` — internal helpers

`oci::detail` holds pure helpers — URL/path builders, checked JSON field
accessors, response-body parsers, token-handshake helpers, docker config.json
helpers. They are deliberately kept out of the public interfaces: include this
header only from `src/oci/*.cpp` and `test/unit/oci_*.cpp`.

Worth knowing about even if you never call them: `json_string_or` and
`json_size_or` exist because registry responses are untrusted input and
nlohmann's `value()` *throws* when a key is present with the wrong type. Reach
for these rather than `value()` when reading anything off the wire.

---

## Use cases

### The common shape

Every workflow, whether reading or writing, follows the same four steps. Only
step 4 differs.

```cpp
// 1. resolve credentials (nullopt == anonymous)
auto creds = oci::resolve_credentials(host, sources);

// 2. split the configured registry url into base + repository prefix.
//    registry_cfg.url is already a util::url — parsed when the config was
//    read — so this cannot fail.
const auto loc = oci::split_registry(registry_cfg.url);   // {base, prefix}

// 3. build the repository path and bind a client to it
auto repository = oci::repository_path(loc.prefix, nspace, system, uarch,
                                       name, version);
auto client = oci::client::create(loc.base, repository, creds);

// 4. do the work
```

The client is the same object in both directions. It is not a "pull client" or a
"push client": what determines whether a request is authenticated as a reader or
a writer is the *operation*, which internally asks for a `pull` or a `pull,push`
scoped token. A client created with credentials still reads anonymously-available
content without ever fetching a write token, and a client created without
credentials works fine until something needs to write. Callers do not manage any
of this.

Note that credentials are resolved against the **registry**, while a client is
bound to a **repository**. One set of credentials typically outlives several
clients, as `copy_image` shows.

### Pulling an image

Once the client is bound (steps 1–3 above), pulling is: fetch the manifest by
digest, then pull the layers described by it. uenv identifies an image by its
manifest digest — `record.sha` in the local database — so the reference is a
digest, not a tag:

```cpp
const auto image_digest = oci::digest::sha256(record.sha);

auto response = client->get_manifest(oci::reference::digest(image_digest));
if (!response) { /* report response.error() */ }

auto manifest = oci::parse_manifest(response->body);
if (!manifest) { /* report manifest.error() */ }
```

Fetching and parsing are separate steps because the raw bytes and the parsed
model serve different purposes: the bytes carry the image's identity, and the
parsed manifest is what tells you which layers to fetch. The manifest is fetched
once and reused by both pulls below.

A digest reference is a **pin**, and `get_manifest` enforces it: it hashes the
response body and fails unless it matches the requested digest, so `response`
either holds the bytes that were asked for or is an error. This is the trust
anchor of the whole pull — the layer digests that `get_blob_to_file` verifies
against are read out of this body, so a manifest taken on trust would make
every check below it self-referential. `response->digest` is that computed
digest.

Metadata first, since it is small and its absence may be a reason to stop:

```cpp
auto found = oci::pull_meta(*client, image_digest, paths.store);
if (!found) { /* a real error */ }
if (!*found) { /* no metadata attached — warn, or fail if that is all we wanted */ }
```

Then the squashfs, with a progress bar and Ctrl-C handling:

```cpp
auto bar = uenv::make_transfer_bar(record.size_byte, "pulling …");
auto progress = [&bar](std::uint64_t now, std::uint64_t) { bar->update(now); };

bool aborted = false;
util::set_signal_catcher();
auto result = oci::pull_squashfs(*client, *manifest, paths.store, progress,
                                 [&aborted]() {
                                     aborted = aborted || util::signal_raised();
                                     return aborted;
                                 });
```

Two details in that abort predicate are worth copying rather than rediscovering.
`util::signal_raised()` *consumes* the flag, so it must be called exactly once —
latch the result in `aborted` and have the post-download check read the latch, or
the cleanup will be skipped and a partial download left behind. And the abort
path must be distinguished from a genuine failure: on abort, `pull.cpp` raises a
`util::signal_exception` so the surrounding handler removes the partial store
directory and re-raises the signal, rather than printing a download error.

No digest checking appears in this code because there is none to write:
`pull_squashfs` hashes the stream as it lands and fails if it does not match the
layer digest.

See `src/cli/pull.cpp` for the whole flow.

### Pushing an image

Steps 1–3 are identical — same `resolve_credentials`, same `split_registry`,
same `repository_path`, same `client::create`. The only difference from the pull
side is that credentials are now mandatory in practice, since the write token
needs them.

The source image is hashed locally first (`uenv::validate_squashfs_image`), which
gives both the sha and the location of any metadata directory:

```cpp
auto push_tag = oci::tag::parse(*label.tag);

auto push_result = oci::push_squashfs(*client, sqfs->sqfs,
                                      oci::reference::tag(*push_tag),
                                      oci::digest::sha256(sqfs->hash),
                                      progress);
const oci::digest image_digest = *push_result;
```

The image is pushed under a **tag** but comes back identified by a **digest** —
the digest of the manifest bytes just written. That returned digest is the
image's identity from here on.

Passing `oci::digest::sha256(sqfs->hash)` is not an optimisation to skip: the
image was already hashed during validation, and omitting the argument makes
`push_squashfs` read the entire multi-GB file a second time.

Metadata is attached to the image by digest, using the just-returned identity:

```cpp
auto meta_result = oci::attach(*client, oci::reference::digest(image_digest),
                               oci::artifact_type_meta, *sqfs->meta);
```

Note the asymmetry with the pull side: pull addresses the image by digest
because it knows the sha from the database; push addresses it by tag on the way
in and by digest thereafter. `reference` is what lets both use the same client
methods.

On interruption, push needs no cleanup handler — the upload session is not
committed until the manifest is PUT, so a Ctrl-C leaves nothing referencing a
partial blob. This is why `push.cpp` leaves Ctrl-C at its default behaviour while
`pull.cpp` installs a catcher.

See `src/cli/push.cpp`.

### Copying between repositories

`copy_image` is the one workflow that does steps 1–3 for you, because it spans
two repositories and therefore needs two clients:

```cpp
const auto loc = oci::split_registry(registry_cfg.url);
const auto src_repo = oci::repository_path(loc.prefix, src_ns, /* … */);
const auto dst_repo = oci::repository_path(loc.prefix, dst_ns, /* … */);
const auto src_manifest = oci::digest::sha256(src_record.sha);

auto ok = oci::copy_image(loc.base, src_repo, dst_repo, src_manifest,
                          dst_tag, credentials);
```

Internally it shows the two client features that exist for exactly this case.
`add_pull_scope` tells the destination client to request pull scope on the
*source* repository as well, so its tokens are acceptable for a cross-repo
mount — which is why it must be called before the first operation that fetches a
token. `mount_blob` then asks the registry to link the blob rather than move
bytes, returning `false` when the registry declines, at which point the copy
falls back to streaming through local disk. A multi-GB image copy usually
transfers nothing at all.

Because manifests are copied byte-for-byte, the destination image has the same
digest as the source. That is what makes referrers copyable verbatim: a
referrer's `subject` names the image digest, which the copy did not change.

See `src/cli/copy.cpp`.

### Discovering what is attached to an image

```cpp
auto refs = client->referrers(image_digest);
for (const auto& r : *refs) {
    if (r.artifact_type == oci::artifact_type_meta) { /* … */ }
}
```

This replaces `oras discover`. The Referrers API and the tag-schema fallback are
handled inside `referrers`, and an image with no attachments yields an empty
list rather than an error — so any error returned here is a real failure and
should be surfaced, not swallowed.

---

## Tests

- `test/unit/oci_*.cpp` — unit tests for parsers, digests, tags, manifests and
  auth. `oci_registry.cpp` covers the full OCI round-trip against a real
  registry by starting its own [zot](https://zotregistry.dev) instance.
- `test/integration/registry.bats` — end-to-end `uenv push`/`pull` against a
  throwaway local zot registry, driven by the `registry_ctl` and `listing_mock`
  helpers. Self-skips when no zot binary is available.

```bash
./test/unit "[registry]"          # the OCI round-trip unit tests
./test/bats ./test/registry.bats  # end-to-end push/pull
```
