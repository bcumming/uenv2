#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_all.hpp>

#include <util/setns.h>

// process_start_time() underpins namespace_join()'s pid-recycling guard (see
// the tests below): it must be a stable identity for a live process, and
// must fail once a process is gone rather than returning stale/garbage data.

TEST_CASE("process_start_time is stable for a live process", "[setns]") {
    auto a = util::process_start_time(getpid());
    REQUIRE(a);
    auto b = util::process_start_time(getpid());
    REQUIRE(b);
    REQUIRE(*a == *b);
}

TEST_CASE("process_start_time fails once a process has exited", "[setns]") {
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        _exit(0);
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    // reaped, so /proc/pid is gone -- too soon for the kernel to have
    // recycled the pid onto a new process of its own.
    REQUIRE_FALSE(util::process_start_time(pid));
}

// The core of finding 5's fix: namespace_join() must not just trust a pid
// handed to it, it must confirm the process behind it is still the one whose
// start time the caller captured earlier -- this is what stands in for the
// leader having died and the pid being recycled between publication and
// join. This doesn't require any namespace privilege: the mismatch is caught
// before setns() is ever called.
TEST_CASE("namespace_join refuses a pid whose start time no longer matches",
          "[setns]") {
    auto start = util::process_start_time(getpid());
    REQUIRE(start);
    auto r = util::namespace_join(getpid(), "mnt", *start + 1);
    REQUIRE_FALSE(r);
}

TEST_CASE("namespace_join fails for a pid that no longer exists", "[setns]") {
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        _exit(0);
    }
    REQUIRE(waitpid(pid, nullptr, 0) == pid);
    REQUIRE_FALSE(util::namespace_join(pid, "mnt", 0));
}
