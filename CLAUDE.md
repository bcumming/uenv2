# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

uenv2 is a C++20 rewrite of uenv, a tool for managing user environments on HPC systems (specifically CSCS Alps). It provides:
- CLI tool (`uenv`) for managing and running environments from SquashFS images
- Slurm plugin for environment integration with job scheduling
- Optional `squashfs-mount` setuid helper for mounting SquashFS images

The software is deployed as static binaries.
All environment modifications must be done via `uenv run`, `uenv start`, or Slurm integration.

## Build System

This project uses **Meson** (>= 1.3) as its build system with **Ninja** as the backend.

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
- `oras_version=X.Y.Z` - ORAS version to download (default: 1.2.0)

## Testing

Three test suites exist:
1. **unit** - C++ unit tests using Catch2 (in `test/unit/`)
2. **cli** - CLI integration tests using BATS (in `test/integration/cli.bats`)
3. **slurm** - Slurm plugin tests using BATS (in `test/integration/slurm.bats`)
4. **squashfs-mount** - setuid helper tests using BATS (in `test/integration/squashfs-mount.bats`)

### Running Tests

To run the tests, run the tests directly instead of running them through meson.

```bash
# Run tests directly
./test/unit                      # Unit tests
./test/bats ./test/cli.bats     # CLI tests
./test/bats ./test/slurm.bats   # Slurm tests
```

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

- `src/cli/` - CLI command implementations (add_remove, build, copy, delete, find, help, image, inspect, ls, pull, push, repo, run, start, status, completion)
- `src/uenv/` - Core library shared between CLI and Slurm plugin
  - Environment management (`env.h/cpp`, `uenv.h/cpp`)
  - Repository/database operations (`repository.h/cpp`)
  - Parsing (`parse.h/cpp`, `lex.h` in util)
  - Mounting (`mount.h/cpp`)
  - Meta data (`meta.h/cpp`)
  - Container registry interaction (`oras.h/cpp`)
  - Logging (`log.h/cpp`, `print.h/cpp`)
  - Settings management (`settings.h/cpp`)
- `src/util/` - Utility libraries (color, curl, envvars, fs, lex, lustre, semver, shell, signal, strings, subprocess)
- `src/site/` - Site-specific configuration (CSCS-specific logic)
- `src/slurm/` - Slurm plugin implementation
- `src/squashfs-mount/` - Setuid helper for mounting SquashFS images

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
    - the CLI does this by execing the squashfs_mount setuid helper, which in turn runs step 6 below
    - the Slurm plugin performs the mount in the remote context as root before the daemon forks the MPI processes
6. Execute command with environment from view

### Dependencies

All dependencies are built as static libraries via meson wrap:
- CLI11 - command line parsing
- fmt - formatting library
- spdlog - logging
- nlohmann_json - JSON parsing
- sqlite3 - database
- libcurl - HTTP operations
- Catch2 - testing (when tests enabled)
- barkeep - progress indicators (header-only in `extern/`)

The CLI build also downloads the ORAS binary for OCI registry operations.
ORAS is installed in `prefix/libexec` for runtime use by the CLI.

## Development Notes

### Code Style

Always use braces `{}` on `if`, `for`, `while`, and other control flow statements, even for single-statement bodies.

### Error Handling

Use `util::expected<T, E>` (similar to std::expected) for fallible operations. This is defined in `src/util/expected.h`.

### Parsing

The codebase has a custom lexer in `src/util/lex.h/cpp` for tokenizing inputs.
Parse functions are in `src/uenv/parse.h/cpp` and return `util::expected<T, parse_error>`.
All inputs are parsed instead of using regex or simple string processing.
When we have to parse a new input type:
1. update the lexer (if needed)
2. add a `parse` function in `src/uenv/parse.h` and `src/uenv/parse.cpp`
3. write unit tests in `test/unit/parse.cpp`
4. run the unit tests, and repeat the process until they pass.


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
