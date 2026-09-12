#pragma once

#include <string>

#include <util/envvars.h>
#include <util/expected.h>

namespace uenv {

// the local task count and the barrier tag are determined together: a tag
// is only safe to use if it comes from the same source that told us how
// many peers to expect, otherwise the two can disagree (e.g. a shared tag
// paired with a task count guessed some other way) and reproduce the
// unscoped-tag collision this is meant to prevent.
struct join_context {
    int ntasks;
    std::string tag;
};

// determine how many tasks on this node --join should join namespaces
// with, and a tag that all of them independently derive to rendezvous at
// the same barrier. always {1, <pid-unique tag>} when tasks_join is false,
// since mount_and_join_ns skips the barrier entirely for a solo task and
// the tag is otherwise unused.
//
// SLURM only at the moment -- a non-SLURM method can be added here later,
// but note that a common-ancestor-derived tag (e.g. session id + start
// time) is not enough on its own: proc_barrier::create() needs an exact
// nprocs up front, and there is no race-free way to *count* siblings from
// a shared ancestor without a barrier to synchronize the count -- the
// count would have to come from somewhere else (an explicit override, or a
// launcher-specific source), and it would still need to be the same source
// the tag is scoped to.
util::expected<join_context, std::string>
local_join_context(const envvars::state& calling_env, bool tasks_join);

} // namespace uenv
