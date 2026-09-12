#include <cstring>

#include <unistd.h>

#include <spdlog/spdlog.h>

#include <uenv/log.h>
#include <uenv/mount.h>
#include <uenv/mount_kernel.h>
#include <uenv/parse.h>
#include <util/defer.h>

#include "environ.h"
#include "mount_slurm.h"

extern "C" {
#include <slurm/slurm_errno.h>
#include <slurm/spank.h>
}

//
// Implementation
//
namespace impl {

// Performs mounting of the squashfs images inside slurm_spank_init_post_opt in
// the _remote_ context. The squashfs images to mount and their mount points are
// set in the local and allocator contexts, where they are encoded in
// the environment variable UENV_MOUNT_LIST.
// This function relies on this variable being set.
//
// * parse UENV_MOUNT_LIST environment variable if set
// * check that each image:mountpoint is valid
// * perform mount
int init_post_opt_remote(spank_t sp) {
    // initialise logging to be completely disabled
    uenv::init_log(spdlog::level::off);

    // parse environment variables to test whether there is anything to
    // mount
    auto mount_var = uenv::slurm::getenv_wrapper(sp, "UENV_MOUNT_LIST");

    // variable is not set - nothing to do here
    if (!mount_var) {
        return ESPANK_SUCCESS;
    }

    // On NFS filesystems with root_squash, root is mapped to an anonymous
    // unprivileged user, preventing access to the squashfs file. We
    // temporarily adopt the job's effective GID so that file opens succeed
    // for group-readable squashfs files.
    //
    // The job GID is set by Slurm to the user's primary group by default,
    // or to a user-specified group via --gid (Slurm validates membership).
    // If the squashfs file is owned by a group other than the job GID, the
    // user should submit their job with --gid=<group>.
    //
    // Note: mode 600 squashfs files on root_squash NFS are not supported.
    gid_t job_gid;
    if (spank_get_item(sp, S_JOB_GID, &job_gid) != ESPANK_SUCCESS) {
        slurm_error("uenv: failed to get job gid");
        return -ESPANK_ERROR;
    }

    if (setegid(job_gid) != 0) {
        slurm_error("uenv: failed to set effective gid: %s", strerror(errno));
        return -ESPANK_ERROR;
    }
    auto cleanup = util::defer(
        []() noexcept { [[maybe_unused]] int result = setegid(0); });

    // parse and validate the mount descriptions
    // note that it is very important to carefully validate the mount_list
    // * check that the squashfs files exist and can be read by the user
    // * check that the mount points exist
    auto mounts = uenv::parse_and_validate_mounts(mount_var.value());
    if (!mounts) {
        slurm_error("%s", mounts.error().c_str());
        return -ESPANK_ERROR;
    }

    if (auto result = uenv::unshare_as_root(); !result) {
        slurm_error("%s", result.error().c_str());
        return -ESPANK_ERROR;
    }

    if (auto result = uenv::do_mount(mounts.value()); !result) {
        slurm_error("error mounting the requested uenv image: %s",
                    result.error().c_str());
        return -ESPANK_ERROR;
    }

    return ESPANK_SUCCESS;
}

} // namespace impl
