# Changelog

## 10.2.0

- #172 [feature] add a rootless FUSE mounting backend: an unprivileged alternative to the setuid kernel backend.

## 10.1.0

- #174 [fix] generate explicit error when mounting two uenv with the same name
- #173 [improvement] consistent image hashes
- #171 [fix] use openssl sha256 implementation to improve download/upload speeds
- #166 [feature] replace the oras CLI tool with a native OCI registry client

## 10.0.2

- #163 [improvement] add label/squashfs information to uenv status output

## 10.0.1

- #162 [improvement] rename the `UENV_ARG` environment variable to `UENV_LABEL`
- #160 [fix] `uenv status` bug when no views are loaded, and only print active information about the loaded environment
- #159 [improvement] print repo priority in json output of `uenv repo status --json`

## 10.0.0

- #151 [fix] change error to warning when meta data is not attached to images in a registry
- #150 [fix] fix missing headers in gcc 16
- #149 [feature] generate RPMs in github actions
- #148 [feature] support advanced slurm workflows
- #147 [feature] support using '*' as cluster name in configuration files and --system flag
- #145 [feature] move nearly all cscs-specific logic into configuration files
- #144 [fix] fix latent bug parsing date strings
- #143 [feature] improved bash completion for uenv labels and files
- #142 [feature] use toml for configuration; support multiple repositories
- #136 [feature] support for default views

## 9.2.0

- #135 [fix] mount squasfs files on root squashed NFS mounts
- #133 [feature] improved error message for revoked registry tokens
- #131 [feature] remove --list flag in favor of a --format flag
- #129 [feature] `uenv image find` and `uenv image ls` do partial match on uenv names
- #127 [feature] find views, mount point, etc. information using `uenv image inspect` without opening a uenv.

## 9.1.2

- #125 extend `uenv image add` to support adding uenv that are already in a repo
- #126 fix bug looking up the default repository location on non-production systems at CSCS

## 9.1.1

- #120 rename cluster field in elastic logs to avoid name conflict
- #122 clean up `uenv status --format=views` output
- #124 restrict lustre striping to max 32 OST

## 9.1.0

- #116 [feature] add support for lustre striping and cleaning up missing images from repos
- #118 [feature] add `uenv repo migrate` feature

## 9.0.1

- #115 [fix]: fix bash function forwarding bug that broke the module command

## 9.0.0

- #83 [fix] Turn some CLI flags into options, so that they can be set with or without `=`. e.g. `uenv --repo=$HOME/uenv` or `uenv --repo $HOME/uenv`.
- #87 [fix] Only use meta data path in adjacent to a uenv image if it contains an env.json file.
- #90 [fix] `image push` was not pushing the correct meta data path.
- #91 [fix] Add `--json` option to `image ls` and `image find`.
- #93 [fix] a bug where the `--only-meta` flag was ignored on `image pull`.
- #96 [improvement] for unsquashfs to use a single thread when unpacking meta data.
- #97 [improvement] reimplement squashfs-mount in the main repository
- #99 [improvement] improve file name completion in bash.
- #104 [feature] elastic logging.
- #111 [feature] add --format flag to uenv status
- #112 [fix] add hints to error message when uenv is not found
