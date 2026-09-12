#include <optional>

#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <uenv/log.h>
#include <uenv/mount_rootless.h>

#include "environ.h"

extern "C" {
#include <slurm/slurm_errno.h>
#include <slurm/spank.h>
}

//
// Forward declare the implementation of the plugin callbacks.
//
namespace impl {
int slurm_spank_task_init_sqfs_ll(spank_t sp);
} // namespace impl

extern "C" {

int slurm_spank_task_init(spank_t sp, [[maybe_unused]] int ac,
                          [[maybe_unused]] char** av) {
    return impl::slurm_spank_task_init_sqfs_ll(sp);
}

} // extern "C"

namespace impl {

// The FUSE backend mounts per task in slurm_spank_task_init, so there is
// nothing to do in the remote context. This no-op satisfies the shared
// dispatcher in plugin.cpp (the kernel backend provides the real
// implementation in plugin_kernel.cpp).
int init_post_opt_remote(spank_t) {
    return ESPANK_SUCCESS;
}

// use the squashfuse_ll interface
int slurm_spank_task_init_sqfs_ll(spank_t sp) {
    uenv::init_log(spdlog::level::off);

    // parse environment variables to test whether there is anything to
    // mount
    auto mount_var = uenv::slurm::getenv_wrapper(sp, "UENV_MOUNT_LIST");

    // variable is not set - nothing to do here
    if (!mount_var)
        return ESPANK_SUCCESS;

    const uid_t uid = getuid();
    const gid_t gid = getgid();

    uint32_t ntasks = 0;
    uint32_t job_id = 0;
    uint32_t step_id = 0;
    if (spank_get_item(sp, S_JOB_LOCAL_TASK_COUNT, &ntasks) != ESPANK_SUCCESS) {
        slurm_error("uenv: failed to get local task count");
        return -ESPANK_ERROR;
    }
    if (spank_get_item(sp, S_JOB_ID, &job_id) != ESPANK_SUCCESS) {
        slurm_error("uenv: failed to get job id");
        return -ESPANK_ERROR;
    }
    if (spank_get_item(sp, S_JOB_STEPID, &step_id) != ESPANK_SUCCESS) {
        slurm_error("uenv: failed to get job step id");
        return -ESPANK_ERROR;
    }
    const auto barrier_tag = fmt::format("{}-{}", job_id, step_id);

    // parse and validate the mount descriptions
    // note that it is very important to carefully validate the mount_list
    // * check that the squashfs files exist and can be read by the user
    // * check that the mount points exist
    auto mounts = uenv::parse_and_validate_mounts(mount_var.value());
    if (!mounts) {
        slurm_error("%s", mounts.error().c_str());
        return -ESPANK_ERROR;
    }

    if (auto r = uenv::rootless::mount_and_join_ns(
            barrier_tag, static_cast<int>(ntasks), mounts.value(),
            false /*use multi threaded fuse*/, uid, gid);
        !r) {
        slurm_error("%s", r.error().c_str());
        return -ESPANK_ERROR;
    }

    return ESPANK_SUCCESS;
}

} // namespace impl
