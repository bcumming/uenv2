#include <unistd.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <util/expected.h>
#include <util/named_semaphore.h>
#include <util/proc_barrier.h>
#include <util/robust_mutex.h>
#include <util/setns.h>
#include <util/shared_mapping.h>

namespace util {

namespace {

// timeout for waiting on a barrier semaphore.
constexpr std::chrono::seconds barrier_timeout{30};

} // namespace

// The barrier's state: three IPC objects that own themselves (one of them,
// `shared`, holding a fourth -- the robust mutex embedded in shared_state),
// plus the few scalars describing this peer's role in the rendezvous.
class proc_barrier_impl {
  public:
    struct shared_state {
        // guards leader_pid/procs_remaining, and doubles as the "followers
        // wait here until the leader is ready" gate: held by the leader from
        // create() to ready(), and re-taken around the decrement in end().
        // Robust, so a peer that dies while holding it is reported to the
        // next lock() instead of wedging every future peer on this tag.
        robust_mutex setup;
        pid_t leader_pid;
        unsigned long long leader_start_time;
        int procs_remaining;
    };

    proc_barrier_impl(bool is_leader, int nprocs, bool holds_setup,
                      named_semaphore bootstrap, named_semaphore done,
                      shared_mapping<shared_state> shared)
        : is_leader(is_leader), nprocs(nprocs), holds_setup(holds_setup),
          bootstrap(std::move(bootstrap)), done(std::move(done)),
          shared(std::move(shared)) {
    }

    bool is_leader;
    int nprocs;
    bool ended = false;
    bool holds_setup;
    named_semaphore bootstrap; // only used during create(); unlinked in end()
    named_semaphore done;      // followers post, the leader waits
    shared_mapping<shared_state> shared;
};

namespace {

using shared_state = proc_barrier_impl::shared_state;

// Reported by every peer that finds the barrier dead, whichever way it
// finds out: EOWNERDEAD for the first one to lock() after the leader died,
// ENOTRECOVERABLE for all the rest.
constexpr auto barrier_failed =
    "proc_barrier: the leader died before finishing setup; the "
    "barrier has failed";

// Called by whichever peer's setup.lock() discovers the leader died holding
// it. Does NOT unlink the shm/semaphore objects: a not-yet-arrived peer could
// otherwise win the create_exclusive() race and become a bogus second
// leader. They are deliberately abandoned in /dev/shm instead -- same
// accepted trade-off as bootstrap being left held in create() above, but for
// a later failure point (the leader died after create() succeeded, not
// during it).
std::string fail_after_owner_death(robust_mutex& setup) {
    setup.unlock();
    return barrier_failed;
}

} // namespace

// Enter the coordinated section and elect the leader.
util::expected<proc_barrier, std::string> proc_barrier::create(std::string tag,
                                                               int nprocs) {
    if (nprocs < 1) {
        return util::unexpected(
            fmt::format("proc_barrier: nprocs must be >= 1, got {}", nprocs));
    }

    // Serialises access to the shared segment while it is being created and
    // sized: shm_open(O_EXCL) alone picks the leader, but a follower must not
    // open_existing()/mmap() the segment before the leader has finished
    // ftruncate()-ing and initialising it. Not needed again once every peer
    // has passed through this function once, but kept around (and unlinked)
    // alongside the other IPC objects since there is no cheaper safe point to
    // retire it at (see the "why keep it if it's dead weight" discussion).
    auto bootstrap =
        named_semaphore::open(fmt::format("/uenv-run_sem-{}", tag), 1);
    if (!bootstrap) {
        return util::unexpected(bootstrap.error());
    }
    // Counts how many peers have finished acting on the leader; posted once
    // by each follower, waited on by the leader.
    auto done =
        named_semaphore::open(fmt::format("/uenv-run_sem_ready-{}", tag), 0);
    if (!done) {
        return util::unexpected(done.error());
    }
    if (auto r = bootstrap->wait(barrier_timeout); !r) {
        return util::unexpected(r.error());
    }
    // Deliberately not released on any of the error returns below
    // (create_exclusive/setup.init/setup.lock failing). A peer failing here
    // means node-wide resource exhaustion (ENOSPC/EMFILE/ENOMEM), not a bug
    // in one peer -- and any peer of this barrier failing must fail the
    // whole rendezvous, never let the survivors quorate without it. Leaving
    // bootstrap held at 0 is what enforces that: every other peer, whenever
    // it arrives, blocks on its own wait() above and times out instead of
    // racing past this point to a partial rendezvous. The cost is a
    // barrier_timeout-bounded delay and abandoned /dev/shm objects for this
    // tag, the same accepted trade-off as the residual leak in
    // fail_after_owner_death() below. Do not "fix" this by posting bootstrap
    // back on these paths -- that would let a peer arriving after the
    // failure retry create_exclusive() and succeed without the failed peer.

    // Am I the leader? Whoever creates the shared memory object wins; the
    // rest find it already there.
    const auto shm_name = fmt::format("/uenv-run_shm-{}", tag);
    auto created = shared_mapping<shared_state>::create_exclusive(shm_name);
    if (!created) {
        return util::unexpected(created.error());
    }

    const bool is_leader = created->has_value();
    shared_mapping<shared_state> shared;
    bool holds_setup = false;
    if (is_leader) {
        spdlog::trace("barrier: set as leader PID {}", getpid());
        shared = std::move(**created);
        if (auto r = shared->setup.init(); !r) {
            return util::unexpected(r.error());
        }
        auto locked = shared->setup.lock();
        if (!locked) {
            return util::unexpected(locked.error());
        }
        holds_setup = true;
        // seed the peer count here rather than in ready(), so that a leader
        // that fails before it gets that far still leaves a count the
        // remaining peers can decrement to zero and clean up behind.
        shared->procs_remaining = nprocs;
        shared->leader_pid = 0;
        if (auto r = bootstrap->post(); !r) {
            return util::unexpected(r.error());
        }
    } else {
        spdlog::trace("barrier: set as follower");
        auto opened = shared_mapping<shared_state>::open_existing(shm_name);
        if (!opened) {
            return util::unexpected(opened.error());
        }
        shared = std::move(*opened);
        if (auto r = bootstrap->post(); !r) {
            return util::unexpected(r.error());
        }

        // Block here until the leader calls ready() -- or, if it died first,
        // fail the whole barrier cleanly instead of wedging this tag.
        auto locked = shared->setup.lock();
        if (!locked) {
            return util::unexpected(locked.error());
        }
        if (*locked == robust_mutex::owner_state::previous_owner_died) {
            return util::unexpected(fail_after_owner_death(shared->setup));
        }
        // a peer got here first and left the mutex unrecoverable: same
        // failure, and nothing to unlock since lock() did not acquire it.
        if (*locked == robust_mutex::owner_state::unrecoverable) {
            return util::unexpected(barrier_failed);
        }
        if (auto r = shared->setup.unlock(); !r) {
            return util::unexpected(r.error());
        }
    }

    return proc_barrier{std::make_unique<proc_barrier_impl>(
        is_leader, nprocs, holds_setup, std::move(*bootstrap), std::move(*done),
        std::move(shared))};
}

proc_barrier::proc_barrier(std::unique_ptr<proc_barrier_impl> impl)
    : impl_(std::move(impl)) {
}

proc_barrier::proc_barrier(proc_barrier&&) noexcept = default;
proc_barrier& proc_barrier::operator=(proc_barrier&&) noexcept = default;

// Leaves the coordinated section if the caller did not. The IPC handles
// themselves are closed by impl_'s members.
proc_barrier::~proc_barrier() {
    if (impl_ && !impl_->ended) {
        if (auto r = end(); !r) {
            spdlog::warn("barrier: end failed during cleanup: {}", r.error());
        }
    }
}

bool proc_barrier::is_leader() const {
    return impl_->is_leader;
}

pid_t proc_barrier::leader_pid() const {
    return impl_->shared->leader_pid;
}

unsigned long long proc_barrier::leader_start_time() const {
    return impl_->shared->leader_start_time;
}

// Leader only. Publish the pid and its start time, then release the
// followers blocked in create().
util::expected<void, std::string>
proc_barrier::ready(std::optional<pid_t> pid) {
    spdlog::trace("barrier: leader initializing shared data");
    const pid_t p = pid.value_or(getpid());
    auto start_time = process_start_time(p);
    if (!start_time) {
        return util::unexpected(start_time.error());
    }
    impl_->shared->leader_pid = p;
    impl_->shared->leader_start_time = *start_time;
    if (auto r = impl_->shared->setup.unlock(); !r) {
        return r;
    }
    impl_->holds_setup = false;
    return {};
}

// Leader only. Block until every follower has called signal_done().
util::expected<void, std::string> proc_barrier::wait_peers() {
    for (int i = 0; i < impl_->nprocs - 1; ++i) {
        if (auto r = impl_->done.wait(barrier_timeout); !r) {
            return r;
        }
    }
    spdlog::trace("barrier: all peers done");
    return {};
}

// Follower only. Signal that this peer is done acting on the leader.
util::expected<void, std::string> proc_barrier::signal_done() {
    return impl_->done.post();
}

// Leave the coordinated section. Called by every peer (leader and followers)
// once fully done with the barrier. This is the protocol half of the teardown:
// the handles are closed by the destructors of impl_'s members, so an error
// here leaks nothing.
util::expected<void, std::string> proc_barrier::end() {
    if (impl_->ended) {
        return {};
    }
    // set before doing anything, so that neither a failure below nor the
    // destructor can run the protocol a second time.
    impl_->ended = true;

    if (!impl_->holds_setup) {
        auto locked = impl_->shared->setup.lock();
        if (!locked) {
            return util::unexpected(locked.error());
        }
        if (*locked == robust_mutex::owner_state::previous_owner_died) {
            return util::unexpected(
                fail_after_owner_death(impl_->shared->setup));
        }
        if (*locked == robust_mutex::owner_state::unrecoverable) {
            return util::unexpected(barrier_failed);
        }
        impl_->holds_setup = true;
    }

    impl_->shared->procs_remaining--;
    spdlog::trace("barrier: {} peers left excluding myself",
                  impl_->shared->procs_remaining);

    if (impl_->shared->procs_remaining <= 0) {
        spdlog::trace("barrier: cleaning up IPC resources");
        if (impl_->shared->procs_remaining != 0) {
            return util::unexpected(
                fmt::format("barrier: expected 0 peers left but found {}",
                            impl_->shared->procs_remaining));
        }
        if (auto r = impl_->bootstrap.unlink(); !r) {
            return r;
        }
        if (auto r = impl_->done.unlink(); !r) {
            return r;
        }
        if (auto r = impl_->shared.unlink(); !r) {
            return r;
        }
    }

    if (auto r = impl_->shared->setup.unlock(); !r) {
        return r;
    }
    impl_->holds_setup = false;

    spdlog::trace("barrier: done");
    return {};
}

} // namespace util
