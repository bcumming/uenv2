#include <filesystem>
#include <string>
#include <vector>

#include <err.h>
#include <fcntl.h>
#include <sched.h>

#include <linux/loop.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <fmt/format.h>
#include <libmount/libmount.h>
#include <spdlog/spdlog.h>

#include <uenv/mount.h>
#include <uenv/mount_kernel.h>
#include <uenv/parse.h>
#include <util/expected.h>

namespace uenv {

util::expected<void, std::string> unshare_and_become_root() {
    if (unshare(CLONE_NEWNS) != 0) {
        return util::unexpected("Failed to unshare the mount namespace");
    }

    if (auto r = uenv::mount(std::nullopt, "/", std::nullopt, MS_SLAVE | MS_REC,
                             nullptr);
        !r) {
        return r;
    }

    // Set real user to root before creating the mount context, otherwise it
    // fails.
    if (setreuid(0, 0) != 0) {
        return util::unexpected("Failed to setreuid");
    }

    // Makes LIBMOUNT_DEBUG=... work.
    mnt_init_debug(0);
    return {};
}

util::expected<void, std::string> return_to_user_and_no_new_privs(uid_t uid) {
    // set real, effective, saved user id back to the calling user.
    if (setresuid(uid, uid, uid) != 0) {
        return util::unexpected("setresuid failed");
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return util::unexpected("PR_SET_NO_NEW_PRIVS failed");
    }
    return {};
}

util::expected<void, std::string>
do_mount(const std::vector<mount_pair>& mount_entries) {
    if (mount_entries.size() == 0) {
        return {};
    }

    for (auto& entry : mount_entries) {
        std::string mount_point = entry.mount;
        std::string squashfs_file = entry.sqfs;

        // Check the mount point exists inside the mount loop, because the
        // mount point may have been created inside a previous mount.
        if (!std::filesystem::is_directory(mount_point)) {
            return util::unexpected("the mount point is not a valid path: " +
                                    mount_point);
        }

        auto cxt = mnt_new_context();

        if (mnt_context_disable_mtab(cxt, 1) != 0) {
            return util::unexpected("Failed to disable mtab");
        }

        if (mnt_context_set_fstype(cxt, "squashfs") != 0) {
            return util::unexpected("Failed to set fstype to squashfs");
        }

        if (mnt_context_append_options(cxt, "loop,nosuid,nodev,ro") != 0) {
            return util::unexpected("Failed to set mount options");
        }

        if (mnt_context_set_source(cxt, squashfs_file.c_str()) != 0) {
            return util::unexpected("Failed to set source");
        }

        if (mnt_context_set_target(cxt, mount_point.c_str()) != 0) {
            return util::unexpected("Failed to set target");
        }

        // https://ftp.ntu.edu.tw/pub/linux/utils/util-linux/v2.38/libmount-docs/libmount-Mount-context.html#mnt-context-mount
        const int rc = mnt_context_mount(cxt);
        const bool success = rc == 0 && mnt_context_get_status(cxt) == 1;
        if (!success) {
            char code_buf[256];
            mnt_context_get_excode(cxt, rc, code_buf, sizeof(code_buf));
            const char* target_buf = mnt_context_get_target(cxt);
            // careful: mnt_context_get_target can return NULL
            std::string target = (target_buf == nullptr) ? "?" : target_buf;

            return util::unexpected(target + ": " + code_buf +
                                    fmt::format("{}:{}", __FILE__, __LINE__));
        }
    }

    return {};
}
} // namespace uenv
