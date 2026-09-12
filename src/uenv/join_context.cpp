#include <uenv/join_context.h>

#include <unistd.h>

#include <fmt/core.h>

#include <util/tasks_per_node.h>

namespace uenv {

util::expected<join_context, std::string>
local_join_context(const envvars::state& calling_env, bool tasks_join) {
    if (!tasks_join) {
        return join_context{1, fmt::format("squashfs-mount-{}", getpid())};
    }

    const auto tasks_per_node = calling_env.get("SLURM_STEP_TASKS_PER_NODE");
    const auto node_id = calling_env.get("SLURM_NODEID");
    const auto job_id = calling_env.get("SLURM_JOBID");
    const auto step_id = calling_env.get("SLURM_STEPID");
    if (!tasks_per_node || !node_id || !job_id || !step_id) {
        return util::unexpected(
            "--join requires SLURM_STEP_TASKS_PER_NODE, SLURM_NODEID, "
            "SLURM_JOBID and SLURM_STEPID to be set in order to determine "
            "how many tasks to join and a tag unique to this job step");
    }

    auto local_tasks = util::local_rank_count(*tasks_per_node, *node_id);
    if (!local_tasks) {
        return util::unexpected(fmt::format(
            "unable to determine local task count from "
            "SLURM_STEP_TASKS_PER_NODE='{}' SLURM_NODEID='{}': {}",
            *tasks_per_node, *node_id, local_tasks.error().message()));
    }

    return join_context{static_cast<int>(*local_tasks),
                        fmt::format("squashfs-mount-{}-{}", *job_id, *step_id)};
}

} // namespace uenv
