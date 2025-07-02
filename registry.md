Registry interface
- concrete items that exist in a registry are represented by manifests
    - manifests are concretised using find
    - find can return multiple manifests?
        - if the registry supports search a partially complete

`manifest` is being used to describe:
    - a concrete manifest in a registry: `digest`, `optional<meta>`, `url`
    - the manifest JSON (could be from the registry, or for a squashfs file on the FS)
    - isolated squashfs file that is being pushed/added
    - squashfs file and its meta in a repository

in a repository:
- only the manifest is needed to look up the image
    - repo/images/${manifest.digest}
in a registry:
- url + label + manifest to fully resolve
    - ${url}/namespace/${label}@sha256:${manifest.digest}


Issues:
- manifests are tied to an artifact:
    - both registry and repository can apply multiple labels to the same artifact
    - so label can't be stored inside any form of manifest
- you need 
    - ${url}/namespace/${label}@${manifest.sha256}
- namespace makes things awkward
    - because they are not part of the label or url
    - internally uenv stores them outside the label definition
        - the don't belong in there: furthermore we should probably remove system from the label
    - do we include them as part of the url, i.e. `url={base_url}/{namespace}`
- system makes things awkward
- don't add `repo` logic to `registry.pull` because the pull functionality is only called in `uenv image pull`
    - the pull interface should allow `src/cli/pull.cpp` pull individual items (like it currently does)
    - `registry` should be completely unaware of `repository`: its interface should not consume repository, because that is uneccesary abstraction (interactions betwee the two are in `image push/pull` only, so leave that logic at the highest layer where it makes debugging easier).

## artifacts

Artifacts that we want to download (and upload)
- squashfs images
- meta directories
- manifests

Future artifacts:
- squashfs files for "modular components", e.g. gcc/cuda.
- deprecation notices

The squashfs image is the keystone artifact:
- pushed directly to the tag
- it is the one and only one layer in the manifest attached to the tag
- the meta is associated via `attach`/`discover`

We need to pull and push squashfs and meta
- push: `oras push` and `oras attach` for squashfs and meta respectively
- pull: `oras blob fetch --output=` or oras 

| operation |   squashfs                       | meta           |
| --------- |   ---------                      | ---------      |
| push      | `oras push`                      | `oras attach`  |
| pull      | `oras pull`, `oras blob fetch`   | `oras pull`    |


### Pulling

| operation |   squashfs                       | meta           |
| --------- |   ---------                      | ---------      |
| pull      | `oras pull`, `oras blob fetch`   | `oras pull`    |

Calls to `oras pull` are always of the form

`oras pull --output=$store $url/$rep@$digest`

where:
- digest is the digest of the squashfs or meta artifact we want to download (not the digest of the manifest)
- `--output` specifies the path into which the download is performed: `$store/store.squashfs` and `$store/meta` will be created

In the current implementation
-`pull_digest` uses this pattern to download the meta path
-`pull_tag` uses this pattern to download the meta path
- both call `oras pull` with same arguments, except `pull_tag` calls asynchronously and maintains a progress bar.

Conclusions:
- use `oras pull` to pull artifacts associated with a uenv
- always of the form `oras pull $url/$repo@$digest`
- digest is the sha256 of the individual artifacts, found by:
    - squashfs: `oras manifest fetch`
    - meta: `oras discover`

```
// the follow should provide enough information:
manifest = registry.find(label)

manifest = {
    date = oras_manifest.date
    url = $rego/$system/$uarch/$name/$version
    digest // oras resolve $url:$tag
    squashfs = {
        digest = oras.manifest.layers[0].digest
        size   = oras.manifest.layers[0].size
    }
    meta = {
        digest = oras.discover.digest
        size   = null
    }
}

// squashfs and meta are:
struct blob {
    sha256 digest;
    string filename;
    string url
    optional<size_t> size;
}

```

The API question becomes:

should `manifest = registry.find()` return
- a list of fully self-contained artifacts (i.e. blob contains url and date information)
- a struct that raises common fields like date and url, and contains a list of artifacts
- both: the struct with common data, that is also replicated inside each blob.
- and should there be a list (variable length array), or struct with named fields (.squashfs, .meta) that can be referred to by name?

We will need to:
- ask "does the image provide meta?"
- iterate over all artifacts

CONCLUSIONS:
- a registry artifact is 
```
// a registry is a very simple artifact:
registry {
    optional<credential_state>
    string url // $rego_url/$nspace
}

manifest = registry_find(registry&, label);
if (!manifest) {error}

registry_pull(registry, target_path, manifest.squashfs)
if manifest.meta:
    registry_pull(registry, target_path, manifest.meta)

record = {label, manifest.digest}
repo.update()
```

### Pushing

| operation |   squashfs                       | meta           |
| --------- |   ---------                      | ---------      |
| push      | `oras push`                      | `oras attach`  |

Calls to `oras push` are of the form
```
oras push
    --artifact-type=application/x-squashfs
    $url/$rep:tag path
    ./store.squashfs
```

Additionally you can set `--concurrency=10`.

pushing meta data:
```
oras attach
    --artifact-type=uenv/meta
    $url/$rep:tag path
    ./meta
```

Both haev the same format of `oras $op --artifact-type remote local`.

IDEA: any representation of a uenv, be it on the local filesystem or remote:
- provide sha and size of the squashfs artifact-type
- provide a list of attachments
    - type (uenv-meta, etc.)
    - sha
    - name on disk

For push and pull, we need to provide different meta-data.

## operations

push
- source (squashfs / path), manifest, label
- `push(prefix, source_path, repo_manifest, label)`
pull
- manifest, destination (repo), label
- `pull(prefix, rego_manifest, label)`
copy

delete
- manifest + label
- label : repo knows the 

```
// a manifest created/used by oras is basically
// - empty config.yaml
// - a single layer for the squashfs
//      [digest, size, type]
// - a single annotation
struct oci_manifest {
    sha256 digest;
    std::string manifest_json;
    size_t size; // part of manifest?
};

// represents a concrete artifact in repository
struct repo_manifest {
    oci_manifest oci;
    // path inside the repository where store.squashfs+meta+[manifest.yaml] is maintained
    fs::path path;
    uenv_label label;
}

// represents a concrete artifact in registry
struct registry_manifest {
    oci_manifest oci;
    // the uenv-label is required to fully resolve the repository
    uenv_label label;
    string url;
    // final url: {url}/{label.system}/{label.uarch}/{label.name}/{label.version}@sha256:{oci.digest}
}

struct registry {
    std::optinal<credentials> credentials;

    registry();
    registry(credentials);

    manifest find(registry);
    manifest create(squashfs);

    manifest find(label)

    push(label, manifest)
    delete(label, manifest)

    // pull a manifest to a repository
    pull(manifest, repository)

    copy(manifest source, label destination)
}
```

extend `repository` to return a manifest from `image add` -> 

## modular uenv

Storage format:

cuda, gcc, etc can be stored as blobls

rego/blobs/uarch/name/version:tag

jfrog/blobs/gh200/cuda/12.6:v1
or
jfrog/blobs/gh200/cuda:12.6

images:
jfrog/env/gh200/prgenv-gnu/25.6:v3

How to represent an image?
How about breaking it apart

```
uarch: gh200,
system: daint,
mounts: [
    cuda/12.6:v2, /uenv/cuda;
    gcc/13.3:v1,  /uenv/gcc;
    cray-mpich/9.0:v1, /uenv/comms;
    prgenv-gnu/25.1:v2, /uenv/env;
    $SCRATCH/squashfs/mydata.squashfs, $SCRATCH/data;
    $SCRATCH/squashfs/tools.squashfs, $SCRATCH/tools;
],
variables: [
    [PATH, prepend, $SCRATCH/tools/bin],
    ...
],
```
