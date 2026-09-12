#include <unistd.h>

#include <catch2/catch_all.hpp>
#include <fmt/core.h>

#include <uenv/join_context.h>
#include <util/envvars.h>

TEST_CASE("local_join_context: --join not requested", "[join_context]") {
    envvars::state env{};
    auto r = uenv::local_join_context(env, false);
    REQUIRE(r);
    CHECK(r->ntasks == 1);
    CHECK(r->tag == fmt::format("squashfs-mount-{}", getpid()));
}

TEST_CASE("local_join_context: --join with no SLURM variables set",
          "[join_context]") {
    envvars::state env{};
    auto r = uenv::local_join_context(env, true);
    REQUIRE_FALSE(r);
}

TEST_CASE("local_join_context: --join with a complete SLURM environment",
          "[join_context]") {
    envvars::state env{};
    env.set("SLURM_STEP_TASKS_PER_NODE", "5,4(x3)");
    env.set("SLURM_NODEID", "0");
    env.set("SLURM_JOBID", "42");
    env.set("SLURM_STEPID", "3");

    auto r = uenv::local_join_context(env, true);
    REQUIRE(r);
    CHECK(r->ntasks == 5);
    CHECK(r->tag == "squashfs-mount-42-3");
}

TEST_CASE("local_join_context: tag tracks job/step, ntasks tracks node",
          "[join_context]") {
    envvars::state env{};
    env.set("SLURM_STEP_TASKS_PER_NODE", "5,4(x3)");
    env.set("SLURM_NODEID", "1");
    env.set("SLURM_JOBID", "7");
    env.set("SLURM_STEPID", "0");

    auto r = uenv::local_join_context(env, true);
    REQUIRE(r);
    CHECK(r->ntasks == 4);
    CHECK(r->tag == "squashfs-mount-7-0");

    // a different job/step on the same node gets a different tag, so two
    // unrelated --join invocations sharing a node cannot collide.
    env.set("SLURM_JOBID", "8");
    auto r2 = uenv::local_join_context(env, true);
    REQUIRE(r2);
    CHECK(r2->tag != r->tag);
}

TEST_CASE("local_join_context: --join with each SLURM variable missing in "
          "turn is a hard error",
          "[join_context]") {
    auto full_env = [] {
        envvars::state env{};
        env.set("SLURM_STEP_TASKS_PER_NODE", "5,4(x3)");
        env.set("SLURM_NODEID", "0");
        env.set("SLURM_JOBID", "42");
        env.set("SLURM_STEPID", "3");
        return env;
    };

    for (const auto* missing : {"SLURM_STEP_TASKS_PER_NODE", "SLURM_NODEID",
                                "SLURM_JOBID", "SLURM_STEPID"}) {
        auto env = full_env();
        env.unset(missing);
        REQUIRE_FALSE(uenv::local_join_context(env, true));
    }
}

TEST_CASE("local_join_context: malformed SLURM_STEP_TASKS_PER_NODE is a "
          "hard error",
          "[join_context]") {
    envvars::state env{};
    env.set("SLURM_STEP_TASKS_PER_NODE", "not-a-partition-string");
    env.set("SLURM_NODEID", "0");
    env.set("SLURM_JOBID", "42");
    env.set("SLURM_STEPID", "3");

    REQUIRE_FALSE(uenv::local_join_context(env, true));
}

TEST_CASE("local_join_context: SLURM_NODEID out of range is a hard error",
          "[join_context]") {
    envvars::state env{};
    env.set("SLURM_STEP_TASKS_PER_NODE", "5,4(x3)");
    env.set("SLURM_NODEID", "4"); // only nodes 0-3 are described
    env.set("SLURM_JOBID", "42");
    env.set("SLURM_STEPID", "3");

    REQUIRE_FALSE(uenv::local_join_context(env, true));
}
