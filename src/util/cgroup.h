#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace util {

// The contents of /proc/self/cgroup, with trailing whitespace removed, or
// std::nullopt if it cannot be read.
std::optional<std::string> current_cgroup();

// Does this cgroup path (the contents of /proc/<pid>/cgroup) describe a
// process that Slurm tracks with cgroups, i.e. one that slurmstepd will
// signal when its job step ends? Matches `job_<id>/step_<n>` (cgroup v1, and
// some v2 deployments) or a `slurmstepd.scope` scope with an opaque job/step
// name (other v2 deployments). Anything else is tracked some other way
// (proctrack/linuxproc, proctrack/pgid), where a process reparented away
// from its task is not reliably reaped.
bool cgroup_is_slurm_managed(std::string_view cgroup);

} // namespace util
