#include <catch2/catch_all.hpp>

#include <util/cgroup.h>

TEST_CASE("cgroup_is_slurm_managed", "[cgroup]") {
    // cgroup v2: a single line, as observed on Alps with
    // ProctrackType=proctrack/cgroup
    REQUIRE(util::cgroup_is_slurm_managed(
        "0::/system.slice/slurmstepd.scope/job_4530470/step_75/user/task_0"));
    // the batch step and the extern step are named the same way
    REQUIRE(util::cgroup_is_slurm_managed(
        "0::/system.slice/slurmstepd.scope/job_17/step_batch/user/task_0"));

    // cgroup v1: one line per controller
    REQUIRE(util::cgroup_is_slurm_managed(
        "11:freezer:/slurm/uid_1000/job_17/step_0\n"
        "10:memory:/slurm/uid_1000/job_17/step_0/task_0\n"
        "0::/user.slice"));

    // not tracked with cgroups: proctrack/linuxproc, proctrack/pgid, or not
    // under Slurm at all
    REQUIRE(!util::cgroup_is_slurm_managed("0::/user.slice/user-1000.slice"));
    REQUIRE(!util::cgroup_is_slurm_managed("0::/"));
    REQUIRE(!util::cgroup_is_slurm_managed(""));

    // "job_" has to be a path component, not a suffix of an unrelated one
    REQUIRE(!util::cgroup_is_slurm_managed("0::/user.slice/myjob_17/step_0"));

    // cgroup v2, opaque job/step name under slurmstepd.scope
    REQUIRE(util::cgroup_is_slurm_managed(
        "0::/system.slice/slurmstepd.scope/s3F9M0T59DSZ00/step_2/user/"
        "task_special"));

    // "slurmstepd.scope" has to be a path component, not a substring
    REQUIRE(!util::cgroup_is_slurm_managed(
        "0::/system.slice/myslurmstepd.scope/s3F9M0T59DSZ00/step_0"));
}

TEST_CASE("current_cgroup", "[cgroup]") {
    const auto cgroup = util::current_cgroup();
    // /proc/self/cgroup always has at least one line on Linux
    REQUIRE(cgroup);
    REQUIRE(cgroup->back() != '\n');
}
