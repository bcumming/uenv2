#pragma once

#include <string>
#include <sys/types.h>

#include <util/expected.h>

namespace util {

// Coordinates a fork(2) where the child does not exec another program but
// instead keeps running arbitrary code in-process (e.g. becoming a
// long-lived daemon), and the parent needs to block until the child has
// finished its setup and is ready to serve. A pipe(2) created before
// fork() carries a one-shot, one-directional "ready" signal from child to
// parent
class ready_fork {
  public:
    ready_fork(const ready_fork&) = delete;
    ready_fork& operator=(const ready_fork&) = delete;
    ready_fork(ready_fork&&) = delete;
    ready_fork& operator=(ready_fork&&) = delete;
    ~ready_fork();

    // public only so that expected<ready_fork, std::string>'s in_place
    // construction (see create()) can reach it -- prefer create() at call
    // sites.
    ready_fork(int read_fd, int write_fd, pid_t parent_pid);

    // creates the pipe(2) backing the handshake. The only failure mode is
    // pipe(2) itself (e.g. EMFILE).
    static expected<ready_fork, std::string> create();

    // wraps fork(2); closes the end of the pipe this process will not use
    // (the write end in the parent, the read end in the child). Same
    // return contract as fork(2): 0 in the child, the child's pid in the
    // parent, -1 on failure.
    pid_t fork();

    // child-side; the pid of the process that called create() -- captured
    // there, since create() is documented to run in the parent before
    // fork(). A child arming a parent-death signal (e.g. PR_SET_PDEATHSIG)
    // must check getppid() against this right after arming it: prctl(2)'s
    // documented race is that the parent can die in the gap between fork()
    // returning and the signal actually being armed, in which case the
    // child is already silently reparented and nothing will ever signal it.
    pid_t parent_pid() const;

    // child-side; call exactly once, right before entering the "serve
    // forever" loop, only once setup has fully succeeded. Returns an error
    // if the write to the parent fails (e.g. the parent has already died
    // and closed its read end) -- callers that don't arm their own
    // parent-death signal (e.g. PR_SET_PDEATHSIG) should check this rather
    // than assume the child can safely keep running unsupervised.
    expected<void, std::string> notify_ready();

    // parent-side; call exactly once after fork(). Blocks until the child
    // calls notify_ready(), or returns an error if the child exits/dies
    // before signaling (observed as EOF), or if read() itself fails.
    expected<void, std::string> wait_ready();

  private:
    int read_fd_;
    int write_fd_;
    pid_t parent_pid_;
};

} // namespace util
