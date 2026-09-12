# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) and other coding
agents when working with code in this repository. It is the single source of
truth; `AGENTS.md` is a symlink to this file.

## Project Overview

uenv2 is a C++20 rewrite of uenv, a tool for managing user environments on HPC systems (specifically CSCS Alps). It provides:
- CLI tool (`uenv`) for managing and running environments from SquashFS images
- Slurm plugin for environment integration with job scheduling
- Optional `squashfs-mount` helper for mounting SquashFS images, built with one
  of two backends chosen at compile time — a setuid kernel-driver backend, or
  a rootless FUSE backend (see "Mounting backends: kernel vs FUSE" below)

The software is deployed as static binaries.
All environment modifications must be done via `uenv run`, `uenv start`, or Slurm integration.

## Build System

This project uses **Meson** (>= 1.4) as its build system with **Ninja** as the backend.

### Building

```bash
# Configure build (from repository root)
mkdir build && cd build
meson setup -Dtests=enabled ..

# Compile
meson compile

# Install (requires sudo for system installation)
sudo meson install --no-rebuild --skip-subprojects

# Install to staging directory (for testing)
sudo meson install --destdir=$PWD/staging --no-rebuild --skip-subprojects
```

### Build Options

Configure via `-Doption=value` with `meson setup`:
- `tests=enabled|disabled` - Enable test suite (default: disabled)
- `cli=true|false` - Build CLI tool (default: true)
- `slurm_plugin=true|false` - Build Slurm plugin (default: true)
- `squashfs_mount=true|false` - Build squashfs-mount helper (default: false)
- `mount_backend=kernel|fuse` - Which mounting backend the CLI, Slurm plugin,
  and squashfs-mount helper are all built against (default: `kernel`). A
  compile-time, exclusive choice — a single build links one backend, never
  both. See "Mounting backends: kernel vs FUSE" below. `squashfs_mount`'s
  install mode also depends on this: setuid (`rwsr-xr-x`, root-owned) for
  `kernel`, plain executable for `fuse`.

### Subproject options and stale build directories

The `default_options` passed to a `subproject()` call in `meson.build` are applied
**only the first time that subproject is configured**. Editing them and running
`meson setup --reconfigure` does *nothing*: the build directory keeps the option
values it was created with, the build succeeds, and the only symptom is a binary
that was built the old way.

This matters most for the `curl` subproject, where every feature is pinned
explicitly so that the shipped binary has no runtime dependencies beyond libc,
libstdc++, libm and libgcc_s. Most of curl's options default to `auto`, meaning
they switch on wherever the build host happens to have the matching dev package
installed — leaving any of them unpinned makes `ldd` output a property of the
build machine rather than of the source tree.

After changing a subproject's options, recreate the build directory:

```bash
meson setup --wipe build          # or: rm -rf build && meson setup ... build
```

Check the result rather than assuming it took — `meson setup` prints a feature
summary per subproject, and the finished binary should show only the four system
libraries:

```bash
ldd build/uenv
```

CI and the RPM build always start from a fresh directory, so they are unaffected;
this only bites developers with a long-lived local build directory.

Note that `meson setup --wipe` deletes the whole build directory, which fails if
it contains a root-owned `staging/` from an earlier
`sudo meson install --destdir=...`. Remove that with `sudo` first.

## Testing

Five test suites exist:
1. **unit** - C++ unit tests using Catch2 (in `test/unit/`)
2. **cli** - CLI integration tests using BATS (in `test/integration/cli.bats`)
3. **slurm** - Slurm plugin tests using BATS (in `test/integration/slurm.bats`)
4. **squashfs-mount** - setuid helper tests using BATS (in `test/integration/squashfs-mount.bats`)
5. **registry** - `uenv push`/`pull` against a throwaway local zot registry (in `test/integration/registry.bats`); self-skips when no zot binary is available

### Running Tests

To run the tests, run the tests directly instead of running them through meson.

```bash
# Run tests directly
./test/unit                      # Unit tests
./test/bats ./test/cli.bats      # CLI tests
./test/bats ./test/slurm.bats    # Slurm tests
./test/bats ./test/registry.bats # Registry (push/pull) tests
```

### System name in tests

Most CLI/Slurm tests resolve uenvs by a bare label (`app/42.0`, `tool`) and rely
on the default system being `arapiles` (the repo records are stored `@arapiles`).

The default system name comes from the config layers, merged in this order (later
wins): `CLUSTER_NAME` env var → system config (`/etc/uenv/config.toml`) → user
config (`$XDG_CONFIG_HOME/uenv/config.toml` or `$HOME/.config/uenv/config.toml`) →
the `--system` CLI flag. On Alps the deployed system config sets
`system_name = 'eiger'`, which **overrides `CLUSTER_NAME=arapiles`**. So exporting
`CLUSTER_NAME` in a test is not sufficient.

To force the system name, `cli.bats` and `slurm.bats` `setup()` write a throwaway
user config that sets `system_name = 'arapiles'` and point `XDG_CONFIG_HOME` at it
(user config beats system config). A test that writes its own `config.toml` must
include `system_name = 'arapiles'` (or append to the file created in `setup()`
with `>>` rather than clobbering it with `>`). Alternatively, pass `--system` on
the command line.

### Elastic mock (`elastic_mock`)

The slurm tests include an elastic telemetry test that uses a standalone mock server at `test/integration/elastic_mock`. The meson build copies it to `$BUILD_PATH/test/elastic_mock` (with execute permissions), and `setup_suite.bash` adds `$BUILD_PATH/test` to `PATH` so it is available by name in all BATS tests.

The script supports subcommands and is also useful for manual testing:

```bash
# pick a free port, start the server (backgrounds itself with &)
port=$(elastic_mock free-port)
elastic_mock serve /tmp/cap.json "$port" &
elastic_mock wait-server "$port"        # wait until accepting connections

# send a request and inspect results
curl -s -X POST http://127.0.0.1:$port -d '{"name":"tool"}'
elastic_mock count /tmp/cap.json        # → 1
elastic_mock get /tmp/cap.json          # pretty-print last record
elastic_mock assert /tmp/cap.json name tool   # exits 0
elastic_mock assert /tmp/cap.json name wrong  # exits 1 + diagnostic

# stop the server (or use `kill %1` / stop_elastic_mock in BATS)
elastic_mock kill /tmp/cap.json
```

In BATS tests the helper functions in `common.bash` wrap the common lifecycle: `start_elastic_mock CAPTURE_FILE PORT` (backgrounds `serve` and calls `wait-server`), `stop_elastic_mock` (kills by PID), and `wait_elastic_post CAPTURE_FILE [TIMEOUT]`.

### Registry mock (`registry_ctl`, `listing_mock`)

The `registry` suite exercises the native OCI client (`src/oci`) end-to-end against
a real registry, without containers or the old `oras` binary. Two helper scripts
are built into `$BUILD_PATH/test` (and so are on `PATH` in all BATS tests):

- `registry_ctl` — manages the lifecycle of a throwaway [zot](https://zotregistry.dev)
  registry (a single static binary). Subcommands include `runtime` (report
  the zot binary, empty if unavailable → suite self-skips), `free-port`, `serve
  STATE PORT` (backgrounds itself), `wait PORT --timeout N`, `digest PORT REPO REF`,
  and `kill STATE`.
  The zot binary is fetched at build time by `test/integration/install-zot`.
- `listing_mock` — stands in for the CSCS uenv listing service
  (`https://uenv-list.svc.cscs.ch/list`); same `free-port`/`serve`/`wait-server`/`kill`
  lifecycle. Point uenv at it with `registry.listing_url` in the config.

`registry.bats` drives `registry_ctl serve` / `listing_mock serve` directly from
its `setup_file` (the servers must outlive individual tests, so their state is
exported rather than held in `common.bash` helper vars). It writes a user
`config.toml` whose `[registry]` block sets `url`, `default_namespace`, and
`listing_url` to point at the local zot + listing_mock. The `[registry]` unit tests
in `test/unit/oci_registry.cpp` cover the OCI round-trip directly by starting their
own zot.

### Testing squashfs-mount

The `squashfs-mount` helper requires setuid installation to test:

```bash
# Set up staging path
export STAGE=$PWD/staging
export STAGING_PATH=$STAGE/usr/local

# Build with squashfs-mount enabled
meson setup -Dtests=enabled -Dsquashfs_mount=true
meson compile

# Install as root (for setuid bit)
sudo meson install --destdir=$STAGE --no-rebuild --skip-subprojects

# Run tests (STAGING_PATH tells tests where to find the setuid binary)
./test/bats ./test/cli.bats
./test/bats ./test/squashfs-mount.bats
```

**IMPORTANT**: Never build with sudo (`sudo meson compile` or `sudo ninja`). Always build as normal user, then install with sudo.

## Architecture

### Source Structure

- `src/cli/` - CLI command implementations (add_remove, build, completion, config, copy, delete, find, help, image, inspect, ls, pull, push, repo, run, start, status)
- `src/uenv/` - Core library shared between CLI and Slurm plugin
  - Environment management (`env.h/cpp`, `uenv.h/cpp`)
  - Repository/database operations (`repository.h/cpp`)
  - Parsing (`parse.h/cpp`, `lex.h` in util)
  - Mounting (`mount.h/cpp`, plus exactly one of `mount_kernel.h/cpp` or
    `mount_rootless.h/cpp` — chosen by the `mount_backend` build option, see
    "Mounting backends: kernel vs FUSE" below)
  - Multi-task rendezvous tag/count derivation for the FUSE backend
    (`join_context.h/cpp`)
  - Meta data (`meta.h/cpp`)
  - Views (`view.h/cpp`)
  - Telemetry (`telemetry.h/cpp`, `elastic.h/cpp`)
  - Logging (`log.h/cpp`, `print.h/cpp`)
  - Settings management (`settings.h/cpp`)
- `src/oci/` - Native OCI registry client (container registry interaction: pull, push, copy, manifests, auth); replaces the external `oras` binary. See "Self-contained `src/oci`" below.
- `src/util/` - Utility libraries (color, curl, envvars, fs, lex, lustre, semver, shell, signal, strings, subprocess, toml), plus the FUSE backend's IPC/process-coordination primitives (`proc_barrier.h/cpp`, `named_semaphore.h`, `shared_mapping.h`, `robust_mutex.h`, `setns.h/cpp`, `ready_fork.h/cpp`) — see "Multi-task rendezvous and IPC error model" below
- `src/site/` - Site-specific configuration (CSCS-specific logic)
- `src/slurm/` - Slurm plugin implementation; `plugin_kernel.cpp` or `plugin_fuse.cpp` is compiled in depending on `mount_backend`
- `src/squashfs-mount/` - Helper for mounting SquashFS images; built from `squashfs-mount-kernel.cpp` (installed setuid) or `squashfs-mount-fuse.cpp` (installed as a plain executable) depending on `mount_backend`

### Key Concepts

**uenv_label**: Represents a uenv identifier with optional fields:
- Format: `name/version:tag@system%uarch`
- Example: `prgenv-gnu/24.11:v2@daint%gh200`

**uenv_description**: Describes a uenv either by label or by filename, with optional mount point.

**concrete_uenv**: Fully resolved uenv with paths (mount_path, sqfs_path, meta_path) and loaded metadata.

**repository**: SQLite-backed database tracking available uenvs. Operations: query, add, remove, contains.

**view**: Named environment configurations within a uenv (stored in env.json metadata).

### Data Flow

1. User provides uenv description (label or file path)
2. Parse into `uenv_description` using `parse.h` functions
3. Resolve to `concrete_uenv` (find squashfs image, mount location, metadata)
4. Load view from metadata (env.json) and configure update to the environment variables
    - the CLI performs this by updating the environment variable store that is used to create `environ` for the call to exec in step 5
    - the Slurm plugin sets environment variables using setenv/getenv in the local context, and letting Slurm forward the environment to the remote context
5. Mount squashfs image at mount point
    - the CLI does this by execing the `squashfs-mount` helper, which in turn runs step 6 below
    - the Slurm plugin performs the mount in the remote context before the daemon forks the MPI processes
    - on the `kernel` backend, both paths go through the setuid helper as root; on the `fuse` backend, the mount happens rootlessly in a user/mount namespace, and if multiple Slurm tasks share the node, only one ("the leader") actually mounts — see the next two sections
6. Execute command with environment from view

### Mounting backends: kernel vs FUSE

Two mutually exclusive backends implement the actual squashfs mount, selected
by the `mount_backend` build option (`kernel` default, or `fuse`). A single
binary only ever contains one — `meson.build` compiles in exactly one of each
pair (`mount_kernel.cpp`/`mount_rootless.cpp` into the shared library,
`plugin_kernel.cpp`/`plugin_fuse.cpp` into the Slurm plugin,
`squashfs-mount-kernel.cpp`/`squashfs-mount-fuse.cpp` into the helper binary).
There is no runtime switch between them.

- **`kernel`**: mounts the squashfs image via the kernel's loop-device +
  squashfs driver (`libmount`). Requires root for the mount syscalls. The CLI
  path (`uenv run`/`uenv start`) is an unprivileged process, so it execs the
  setuid `squashfs-mount` helper to get there. The Slurm plugin does not go
  through that helper at all: its mount runs inside
  `slurm_spank_init_post_opt` in the remote context (`src/slurm/plugin_kernel.cpp`),
  a hook Slurm itself invokes as root, so `uenv::do_mount()` is called
  directly with the privilege the plugin already has.
- **`fuse`**: mounts via `squashfuse_ll` inside a fresh user + mount
  namespace, entirely unprivileged — no setuid bit needed
  (`src/uenv/mount_rootless.cpp`). Since Slurm can co-locate several tasks
  from the same job/step on one node, and each task's namespace is otherwise
  private, the tasks that share a node rendezvous around a single leader's
  mount (elected among themselves) and the rest join that leader's
  namespaces with `setns()` rather than each mounting independently. That
  rendezvous is what the next section documents.

### Mount lifetime (FUSE backend)

Who owns a mount's lifetime differs between the solo and the `--join` path,
because what the mount should die with differs.

- **Solo (`ntasks == 1`: `uenv run`, `uenv start`)**: the squashfuse daemon is
  `PR_SET_PDEATHSIG`-tied to the process that goes on to `exec` the command.
  There is exactly one user, and this path also has to work outside Slurm (a
  login node), where nothing else would clean up after it.
- **`--join` (`ntasks > 1`)**: the leader forks a **mount supervisor**
  (`fork_mount_supervisor()`/`supervisor_main()` in `mount_rootless.cpp`) which
  forks the daemons, reports readiness over a `util::ready_fork` pipe, detaches
  its stdio and then sleeps forever. The daemons are `PDEATHSIG`-tied to *it*,
  not to any rank.

  The leader is an arbitrary rank, so anchoring the daemons to the leader's
  command — as the code originally did — took the uenv away from every other
  rank on the node the moment that one rank finished, reported only as
  `Transport endpoint is not connected`.

  **What ends the supervisor is Slurm, and that is deliberate.** `--join` only
  exists inside a job step; every process a step creates is in the step's
  cgroup, membership is inherited across `fork` and cannot be escaped from user
  space, and `slurmstepd` `SIGKILL`s whatever remains in that cgroup as soon as
  the last task exits — measured at ~3 ms on Alps, including for a
  signal-ignoring process in its own namespaces. The mount's lifetime is
  therefore the step's lifetime on that node. Forks, orphans, nested containers
  and fd-closing commands are all irrelevant to it, unlike any scheme where
  uenv tries to work out for itself whether anyone still needs the mount.

  Three consequences that must not be "tidied up":

  - the supervisor arms **no** `PR_SET_PDEATHSIG` of its own — outliving the
    task that forked it is the point;
  - it must **never** `setsid()` or otherwise leave the cgroup it was forked
    into, since that cgroup is the entire termination mechanism;
  - it **ignores `SIGTERM`/`SIGINT`/`SIGHUP`**. On a time limit or `scancel`,
    Slurm signals every process in the cgroup and *then* gives the ranks
    `KillWait` seconds to shut down cleanly; a supervisor that died on that
    signal would pull the mount out from under ranks that are still flushing or
    checkpointing.

  It also detaches stdio once the mounts are up, so a process outliving the
  ranks can never hold the step's stdout/stderr pipes open. Mount failures are
  reported before that point.

  Because that termination mechanism is Slurm's rather than uenv's, the leader
  checks for it instead of assuming it: `util::cgroup_is_slurm_managed()`
  (`src/util/cgroup.h`) looks for Slurm's `/job_<id>/` component in
  `/proc/self/cgroup`, present in both the cgroup v1 and v2 layouts. Where it
  is absent — a site running `proctrack/linuxproc` or `proctrack/pgid`, where a
  process reparented away from its task is not reliably reaped — the leader
  warns and mounts in-process instead, falling back to the solo model. A leaked
  fuse daemon and the namespace pinning its image is worse than the mount
  ending with the leader's command.

  Observed on Alps (Slurm 25.05.4, `proctrack/cgroup`, cgroup v2,
  `PrologFlags=...,Contain`): the supervisor lands in
  `.../slurmstepd.scope/job_<id>/step_<n>/user/task_0`. Re-check this on a new
  system or Slurm version.

### Multi-task rendezvous and IPC error model (FUSE backend)

`src/util/proc_barrier.{h,cpp}` implements the rendezvous the `fuse` backend
uses to elect one mount leader among the Slurm tasks sharing a node
(`src/uenv/mount_rootless.cpp`). It's built from POSIX IPC objects named
after a `tag` string: two named semaphores (`bootstrap`, serializing the
leader-election race; `done`, counting followers finished acting on the
leader) plus one POSIX shared-memory segment holding a `robust_mutex`
(`src/util/robust_mutex.h`, `PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST`)
and the peer count/leader pid. `tag` and the peer count (`nprocs`) are always
derived together from the same source (`uenv::local_join_context()` in
`src/uenv/join_context.h`, scoped to `SLURM_JOBID`-`SLURM_STEPID`) — a tag
must never be paired with a peer count computed some other way, or unrelated
groups can collide on the same IPC names.

**The governing correctness rule for this whole subsystem: if any one peer
fails while the barrier is still being set up, the entire rendezvous must
fail for every peer — never let the survivors proceed on fewer than
`nprocs`.** Two mechanisms enforce this, for the two points at which a peer
can fail:

- **After the leader has taken `setup` (the robust mutex) and started
  setup**: if it dies (SIGKILLed, OOM-killed, hits a job time limit — all
  ordinary HPC events), the kernel's robust-mutex machinery reports
  `EOWNERDEAD` to the next `lock()` instead of wedging forever. The peer that
  observes it deliberately does *not* call `pthread_mutex_consistent()`, so
  the mutex stays permanently `ENOTRECOVERABLE` — every peer, present or
  future, that touches this tag's mutex fails cleanly rather than one
  quietly resuming on state nobody finished writing.
- **Before that point** — `create_exclusive()`, `setup.init()`, or the
  leader's first `setup.lock()` failing (all node-resource-exhaustion class:
  `ENOSPC`/`EMFILE`/`ENOMEM`) — there is no mutex yet to report anything, so
  `proc_barrier::create()` deliberately leaves the `bootstrap` semaphore held
  at 0 on every one of those error paths instead of posting it back (see the
  comment at its `wait()` call in `proc_barrier.cpp`). Every other peer,
  whenever it arrives — even long after a transient cause has cleared —
  blocks on its own `wait()` and times out (`barrier_timeout`, 30s) rather
  than racing past this point to a rendezvous that no longer includes the
  failed peer.

**Do not "fix" either of these by releasing the held resource on error.**
Both look, in isolation, like a plain resource leak: an IPC object acquired
and never released on an early-return error path. The natural-looking fix —
post the semaphore back, or `pthread_mutex_consistent()` the mutex, so the
next peer isn't blocked by this one's failure — is wrong here, because it
lets a peer that arrives *after* the failure retry the election and succeed
alongside the survivors, while the peer that actually hit the error has
already returned its own error and exited. The barrier would then quietly
complete with fewer than `nprocs` peers, which is exactly the outcome the
governing rule above forbids. The held resource being unreleased is what
stops that: it is the mechanism, not an oversight. Before changing any error
path in this subsystem (`proc_barrier.cpp`, `shared_mapping.h`,
`robust_mutex.h`, `setns.cpp`), check whether the group's all-or-nothing
guarantee depends on that path leaving the resource held before treating it
as a leak to close.

The accepted cost of both mechanisms is abandoned `/dev/shm` objects
(semaphores + the shared-memory segment) for a tag whose barrier failed this
way, plus up to `barrier_timeout` of latency. This is bounded, not an
unbounded leak: a tag is scoped to one job/step and is never reused within a
job, so nothing ever looks the abandoned names up again; at CSCS specifically
the Slurm prologue clears `/dev/shm` before every job, bounding it further.

### Dependencies

All dependencies are built as static libraries via meson wrap:
- CLI11 - command line parsing
- fmt - formatting library
- spdlog - logging
- nlohmann_json - JSON parsing
- sqlite3 - database
- libcurl - HTTP operations
- zlib - gzip handling in the native OCI registry client (`src/oci`)
- libarchive - tar packing/unpacking of the `uenv/meta` artifact, in-process via `src/util/archive.*` (replaces the external `tar`/`gzip` binaries)
- Catch2 - testing (when tests enabled)
- barkeep - progress indicators (header-only in `extern/`)
- OpenSSL - the TLS backend for libcurl; no uenv code calls it directly

### The OpenSSL subproject is not a meson build

Every other dependency above is built by meson. OpenSSL is not: upstream ships no
meson build, and wrapdb's third-party port is frozen at 3.0.8 (Feb 2023), a series
whose security support ends 2026-09-07. `subprojects/openssl.wrap` is therefore
ours rather than wrapdb's, and `subprojects/packagefiles/openssl/` drives
OpenSSL's own `Configure` + `make`.

Consequences worth knowing:

- **`perl` and `make` are build requirements**, in addition to meson/ninja/g++.
- OpenSSL is configured *and compiled* during `meson setup`, not `meson compile`
  (about 10-40 s on a cold build directory, a second on a warm one). It has to be:
  most of OpenSSL 3.x's public headers are generated from `.h.in` templates, and
  curl probes `openssl/ssl.h` while *it* is being configured; and a `custom_target`
  output cannot be linked into the installed `libcurl` static library.
- Because OpenSSL is linked statically, no distro update can ever patch it. The
  version users run is the version built into the release, so bumping it is a
  security task, not housekeeping. CI asserts the exact version to keep that
  deliberate.
- **Bumping the version**: change the URL, filename, directory and hash in
  `subprojects/openssl.wrap` (verify the hash against the `.sha256` published
  beside the release asset), the `version:` in
  `subprojects/packagefiles/openssl/meson.build`, and the version assertion in
  `.github/workflows/build_and_test.yml`. Nothing else should need to change.
- **Editing `subprojects/packagefiles/openssl/*` has no effect on an already
  extracted subproject** - the packagefiles are copied over the unpacked tarball
  only when it is first extracted. Delete `subprojects/openssl-<version>/` to pick
  changes up.

## Development Notes

### Code Style

Always use braces `{}` on `if`, `for`, `while`, and other control flow statements, even for single-statement bodies.

Do not use banner-style section comments with trailing dashes
(`// --- section ------`). Prefer a three-line block comment:

```cpp
//
// section
//
```

### Error Handling

Use `util::expected<T, E>` (similar to std::expected) for fallible operations. This is defined in `src/util/expected.h`.

### Parsing

The codebase has a custom lexer in `src/util/lex.h/cpp` for tokenizing inputs.
The shared parsing scaffolding — the `parse_error` type, the `PARSE` macro, and
the `parse_string` primitive — lives in `src/util/parse.h/cpp` (namespace `util`)
so that it can be reused by any module.
Parse functions return `util::expected<T, util::parse_error>`. The `uenv` parsers
are in `src/uenv/parse.h/cpp`; the OCI client has its own parsers in
`src/oci/parse.h/cpp` (see "Self-contained `src/oci`" below).
All inputs are parsed instead of using regex or simple string processing.
When we have to parse a new input type:
1. update the lexer (if needed)
2. add a `parse` function in the relevant `parse.h`/`parse.cpp`
3. write unit tests in the matching `test/unit/*.cpp`
4. run the unit tests, and repeat the process until they pass.

### Self-contained `src/oci`

The `src/oci/` module is a native OCI registry client written to replace the
external `oras` binary. It is intended to be reusable and independently testable,
so it must depend **only** on `src/util/` and external libraries (fmt,
nlohmann_json, libcurl, spdlog, zlib).

`src/oci/` must **not** include or depend on `src/uenv/`, `src/site/`, or
`src/cli/`. Code that both `uenv` and `oci` need (for example the lexer and the
parsing scaffolding) lives in `src/util/` precisely so that `oci` never has to
reach up into `uenv`:

- `src/util/lex.*` — the shared tokenizer.
- `src/util/parse.*` — `util::parse_error`, the `PARSE` macro, and
  `util::parse_string`.

Parsing in `src/oci` follows the same lexer-based idiom as the rest of the
codebase; JSON documents are parsed with nlohmann_json, while
`src/oci/parse.*` handles the character-level string types (digests, references,
URLs, the bearer challenge).

Enforcement: this command must return nothing.

```bash
grep -rn '#include <\(uenv\|site\|cli\)/' src/oci/
```

### Environment Variables

Use `envvars::state` to access environment variables (from `src/util/envvars.h`). Available as `settings.calling_environment` in most CLI commands.

The tool follows the philosophy of not reading environment variables directly using `getenv`, instead we grab a read only copy of the environment at startup, stored in `settings.calling_environment`.
When calling `exec` to run a new command with a modified environment, we copy this initial state, modify it, then pass the modified copy to `exec`.

### Logging

Use spdlog for logging. Set verbosity via `settings.verbose`. Format output using fmt library.

### File System Operations

Prefer using functions from `src/util/fs.h` which provide expected-based error handling over raw std::filesystem operations.

### Adding CLI Commands

1. Create header/source in `src/cli/` (e.g., `foo.h`, `foo.cpp`)
2. Implement command function returning `int` (exit code)
3. Add source to `cli_src` array in `meson.build`
4. Register subcommand in `src/cli/uenv.cpp` main function using CLI11
5. Add integration tests in `test/integration/cli.bats`
6. Add unit tests for any new library functions in `test/unit/`

### Testing New Features

- Add unit tests in `test/unit/` for library functions
- Add BATS integration tests in `test/integration/` for CLI behavior
- BATS tests use test data generated in `test/data/` and setup scripts in `test/setup/`
