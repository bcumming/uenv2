#pragma once

#include <sys/types.h>

#include <memory>
#include <optional>
#include <string>

#include <util/expected.h>

namespace util {

// forward-declared pimpl (in the style of util/sha.h's sha256_impl); the
// semaphores and the shared mapping never leak into this header.
class proc_barrier_impl;

// A rendezvous for a group of unrelated processes that independently reach
// the same point: exactly one is elected leader and performs some setup
// while the rest wait, and the last one out releases the shared resources.
//
// The barrier knows nothing about what the setup is. The leader publishes a
// single pid -- its own, or any other it chooses -- and that pid's start
// time, which is all the followers get told about it.
//
// Peers find each other by tag: everyone sharing a tag joins the same
// barrier. The POSIX semaphores and shared memory the barrier is built from
// are named after the tag, so it must be unique to the group (a job and step
// id, for example).
//
// Move-only: obtain one from create() and pass it around by reference.
//
// Protocol:
//
//     auto barrier = proc_barrier::create(tag, nprocs);  // all peers
//     if (barrier->is_leader()) {
//         ... setup ...
//         barrier->ready()        publish the pid, release the followers
//         barrier->wait_peers()   returns once every follower is done
//         ... work that would break followers if done any earlier ...
//     } else {
//         ... act on barrier->leader_pid() ...
//         barrier->signal_done()
//     }
//     barrier->end()              all peers; last one unlinks the IPC objects
class proc_barrier {
  public:
    // All peers. Elects the leader: on return exactly one peer has
    // is_leader() set. Followers stay blocked here until the leader calls
    // ready(). `tag` must be unique to the group; `nprocs` is the number of
    // peers that will join this barrier and must be >= 1, or this returns an
    // error.
    static util::expected<proc_barrier, std::string> create(std::string tag,
                                                            int nprocs);

    ~proc_barrier();
    proc_barrier(proc_barrier&&) noexcept;
    proc_barrier& operator=(proc_barrier&&) noexcept;

    proc_barrier(const proc_barrier&) = delete;
    proc_barrier& operator=(const proc_barrier&) = delete;

    bool is_leader() const;

    // The pid published by the leader in ready(). Only meaningful to a
    // follower, once create() has returned.
    pid_t leader_pid() const;

    // The start time of leader_pid() at the moment it was published in
    // ready() (see util::process_start_time()). Lets a follower detect
    // leader_pid() having been recycled onto an unrelated process before it
    // gets around to acting on it. Only meaningful to a follower, once
    // create() has returned.
    unsigned long long leader_start_time() const;

    // Leader only. Publish pid (its own, if nullopt) and its current start
    // time, then release the followers blocked in create() so they can act
    // on the leader while it is still in the state they need. Must be called
    // before anything that would invalidate that state. Fails if pid's start
    // time cannot be read -- pid must therefore name a process that is still
    // alive at the point ready() is called.
    util::expected<void, std::string>
    ready(std::optional<pid_t> pid = std::nullopt);

    // Leader only. Block until every follower has called signal_done().
    util::expected<void, std::string> wait_peers();

    // Follower only. Signal that this peer is done acting on the leader.
    util::expected<void, std::string> signal_done();

    // Called by every peer (leader and followers) once fully done with the
    // barrier. Decrements the shared peer counter; whichever call brings it to
    // 0 unlinks the IPC objects. Idempotent, and also performed on
    // destruction, where any error can only be logged.
    util::expected<void, std::string> end();

  private:
    explicit proc_barrier(std::unique_ptr<proc_barrier_impl> impl);

    std::unique_ptr<proc_barrier_impl> impl_;
};

} // namespace util
