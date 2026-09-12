#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sched.h>

#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <uenv/mount.h>
#include <uenv/mount_kernel.h>
#include <uenv/parse.h>
#include <util/defer.h>
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

    // Set real user to root before mounting, otherwise it fails.
    if (setreuid(0, 0) != 0) {
        return util::unexpected("Failed to setreuid");
    }
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

namespace {

// Retry an ioctl while it returns EAGAIN: udev and blkid briefly open loop
// devices when their state changes, and the kernel returns EAGAIN while such
// a transient opener holds the device.
// Mirrors util-linux repeat_on_eagain (lib/loopdev.c): 10 tries at 250ms.
template <typename F> int retry_on_eagain(F&& f) {
    constexpr int max_tries = 10;
    int rc = 0;
    for (int i = 0; i <= max_tries; ++i) {
        errno = 0;
        rc = f();
        if (rc == 0 || errno != EAGAIN) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return rc;
}

struct loop_device {
    std::string path;
    // A read-only fd that keeps the device attached until it is mounted:
    // LO_FLAGS_AUTOCLEAR detaches the device as soon as the last reference
    // (open fd or mount) is dropped.
    int fd;
};

// Attach a squashfs file, opened read-only, to a free loop device.
//
// We use direct loop ioctls + mount(2) instead of libmount's context API
// because libmount >= 2.42 marks any setuid process as "restricted" (via
// AT_SECURE / is_privileged_execution) and refuses to mount without a
// matching /etc/fstab entry, even after setreuid(0,0).
//
// The acquire sequence is retried on EBUSY: LOOP_CTL_GET_FREE does not
// reserve the device it returns, and the loop device pool is global to the
// node, so a concurrent process (another job step or uenv invocation) can
// claim the device before our LOOP_CONFIGURE.
util::expected<loop_device, std::string>
attach_loop_device(const std::string& squashfs_file) {
    // Open the backing file exactly once and use this same fd both to validate
    // the image (below) and to bind the loop device (via LOOP_CONFIGURE).
    // Binding the fd we validated - rather than re-opening the path - closes
    // the time-of-check/time-of-use gap that would otherwise let an attacker
    // swap the path between validation and bind.
    //
    // O_NOFOLLOW rejects a final-component symlink swap. squashfs_file is the
    // already-canonicalized path (see make_mount_pair), so under honest use its
    // final component is a regular file and O_NOFOLLOW never triggers; it fires
    // only if the path was replaced by a symlink after canonicalization. This
    // matters because the open happens as root inside the setuid helper, and
    // mirrors util-linux's loopdev symlink-attack fix (LOOPDEV_FL_NOFOLLOW,
    // advisory GHSA-qq4x-vfq4-9h9g). Note this only guards the final path
    // component; intermediate directory-symlink swaps are not covered (neither
    // does the upstream O_NOFOLLOW fix).
    const int sqfs_fd =
        open(squashfs_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (sqfs_fd < 0) {
        return util::unexpected(
            fmt::format("open {}: {}", squashfs_file, strerror(errno)));
    }
    // after LOOP_CONFIGURE the kernel holds its own reference to the backing
    // file, so the fd is closed on every exit path
    auto close_sqfs = util::defer([sqfs_fd] { close(sqfs_fd); });

    // Validate the image on the fd we are about to bind, so what we check is
    // exactly what we mount. make_mount_pair performs the same checks earlier
    // by path for a fast, friendly error, but those are advisory: the path can
    // change between then and now, so the security-relevant check is here.
    struct stat st = {};
    if (fstat(sqfs_fd, &st) != 0) {
        return util::unexpected(
            fmt::format("stat {}: {}", squashfs_file, strerror(errno)));
    }
    if (!S_ISREG(st.st_mode)) {
        return util::unexpected(
            fmt::format("{} is not a regular file", squashfs_file));
    }
    // A valid squashfs file starts with the little-endian magic "hsqs".
    std::array<char, 4> magic = {};
    if (pread(sqfs_fd, magic.data(), magic.size(), 0) !=
            static_cast<ssize_t>(magic.size()) ||
        !(magic[0] == 'h' && magic[1] == 's' && magic[2] == 'q' &&
          magic[3] == 's')) {
        return util::unexpected(
            fmt::format("{} is not a valid squashfs file", squashfs_file));
    }

    constexpr int max_acquire_attempts = 16;
    for (int attempt = 0; attempt < max_acquire_attempts; ++attempt) {
        int ctrl_fd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
        if (ctrl_fd < 0) {
            return util::unexpected(
                fmt::format("open /dev/loop-control: {}", strerror(errno)));
        }
        const int loopnum = ioctl(ctrl_fd, LOOP_CTL_GET_FREE);
        const int get_free_errno = errno;
        close(ctrl_fd);
        if (loopnum < 0) {
            return util::unexpected(
                fmt::format("LOOP_CTL_GET_FREE: {}", strerror(get_free_errno)));
        }

        const std::string loopdev = fmt::format("/dev/loop{}", loopnum);

        // When LOOP_CTL_GET_FREE allocates a new device the /dev/loopN node
        // is created asynchronously by udev, so wait out ENOENT (node not
        // yet created) and EACCES (permissions not yet applied).
        // The device is opened O_RDWR, as required by LOOP_CONFIGURE.
        int loop_fd = -1;
        for (int i = 0; i < 16; ++i) {
            loop_fd = open(loopdev.c_str(), O_RDWR | O_CLOEXEC);
            if (loop_fd >= 0 || (errno != ENOENT && errno != EACCES)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (loop_fd < 0) {
            return util::unexpected(
                fmt::format("open {}: {}", loopdev, strerror(errno)));
        }
        auto close_loop = util::defer([loop_fd] { close(loop_fd); });

        // Bind the backing file and set the flags in one atomic
        // LOOP_CONFIGURE (Linux >= 5.8):
        // - LO_FLAGS_READ_ONLY makes the block device read-only (also
        //   implied by the O_RDONLY backing fd);
        // - LO_FLAGS_AUTOCLEAR detaches the device automatically once the
        //   last reference (mount or open fd) is gone, so no explicit
        //   LOOP_CLR_FD cleanup is needed on any path after this point.
        struct loop_config config = {};
        config.fd = static_cast<unsigned int>(sqfs_fd);
        config.info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
        strncpy(reinterpret_cast<char*>(config.info.lo_file_name),
                squashfs_file.c_str(), LO_NAME_SIZE - 1);

        if (retry_on_eagain(
                [&] { return ioctl(loop_fd, LOOP_CONFIGURE, &config); }) != 0) {
            if (errno == EBUSY) {
                // the device was claimed by another process between
                // LOOP_CTL_GET_FREE and LOOP_CONFIGURE - try the next one
                spdlog::debug("attach_loop_device: {} stolen, retrying",
                              loopdev);
                continue;
            }
            return util::unexpected(fmt::format("{}: LOOP_CONFIGURE: {}",
                                                loopdev, strerror(errno)));
        }

        // Swap the setup fd for a read-only one before mounting: the kernel
        // blocks mounting a block device that has writable openers.
        const int ro_fd = open(loopdev.c_str(), O_RDONLY | O_CLOEXEC);
        if (ro_fd < 0) {
            // closing loop_fd (close_loop) drops the last reference and
            // AUTOCLEAR detaches the device
            return util::unexpected(
                fmt::format("open {} read-only: {}", loopdev, strerror(errno)));
        }
        return loop_device{loopdev, ro_fd};
    }

    return util::unexpected(fmt::format(
        "unable to acquire a free loop device for {} after {} attempts",
        squashfs_file, max_acquire_attempts));
}

} // namespace

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

        auto loop = attach_loop_device(squashfs_file);
        if (!loop) {
            return util::unexpected(
                fmt::format("{}: {}", mount_point, loop.error()));
        }
        // The mount takes its own reference to the loop device, after which
        // the fd is no longer needed; if the mount fails, closing the fd
        // drops the last reference and AUTOCLEAR detaches the device.
        auto close_loop = util::defer([fd = loop->fd] { close(fd); });

        if (::mount(loop->path.c_str(), mount_point.c_str(), "squashfs",
                    MS_RDONLY | MS_NOSUID | MS_NODEV, nullptr) != 0) {
            const int saved_errno = errno;
            const char* hint =
                saved_errno == EINVAL
                    ? " (bad superblock - corrupt or truncated squashfs?)"
                    : "";
            return util::unexpected(fmt::format(
                "failed to mount {} at {} via {}: {}{}", squashfs_file,
                mount_point, loop->path, strerror(saved_errno), hint));
        }
    }

    return {};
}

} // namespace uenv
