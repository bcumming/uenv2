#pragma once

#include <string>

#include <sys/types.h>

#include <fmt/core.h>

#include <uenv/mount.h>
#include <util/expected.h>

namespace uenv {

/// called as root, in slurm-plugin
util::expected<void, std::string> unshare_as_root();

/// setup hook for the setuid squashfs-mount build: unshare the mount
/// namespace and become the real root user, so that squashfs images can be
/// mounted with the kernel driver.
util::expected<void, std::string> unshare_and_become_root();

/// post hook for the setuid squashfs-mount build: return to the calling
/// user and disallow gaining any new privileges.
util::expected<void, std::string> return_to_user_and_no_new_privs(uid_t uid);

/// mount sqfs images, make sure mnt ns has been unshared before calling this
/// function
util::expected<void, std::string> do_mount(const mount_list& mount_entries);

} // namespace uenv
