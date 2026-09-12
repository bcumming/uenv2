#pragma once

#include "uenv/mount.h"

namespace uenv {
namespace rootless {

// Coordinate SquashFS mounting across `ntasks` peer processes that share a
// node: elect a leader (via a proc_barrier tagged `tag`), which performs the
// mounts and drops privileges, while every other task joins the leader's
// user/mount namespaces. When ntasks == 1 the barrier is skipped entirely
// and this process just mounts and drops privileges by itself.
//
// The mount's lifetime is owned by:
//
//   ntasks == 1  the process that execs the command, via PR_SET_PDEATHSIG.
//                There is one user, and this path must also work outside
//                Slurm (a login node), where nothing else would clean up.
//   ntasks > 1   a supervisor process forked by the leader, reaped by
//                slurmstepd's end-of-step sweep of the step's cgroup. The
//                leader is an arbitrary rank, so anchoring the mount to its
//                command would take the uenv away from every other rank as
//                soon as that one finished.
//
// The calling process's dumpable state is captured up front and restored
// once it is safe to do so, so a caller that started out non-dumpable ends
// up non-dumpable again, and one that started out dumpable stays dumpable --
// callers do not need their own policy for this.
util::expected<void, std::string>
mount_and_join_ns(const std::string& tag, int ntasks,
                  const uenv::mount_list& mounts, bool fuse_single_threaded,
                  uid_t uid, gid_t gid);

} // namespace rootless
} // namespace uenv
