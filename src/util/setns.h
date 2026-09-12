#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include <util/expected.h>

namespace util {

// The process's start time (field 22 of /proc/pid/stat, clock ticks since
// boot). Paired with a pid, this is the kernel's own notion of a stable
// process identity: a pid on its own gets recycled, but a (pid, start_time)
// pair does not, since a newly created process is always given a start_time
// no earlier than "now".
util::expected<unsigned long long, std::string> process_start_time(pid_t pid);

// Enter the namespace ns (e.g. "user", "mnt") of the process pid, via
// setns(2). expected_start_time (from process_start_time(), captured by the
// caller before pid was published to us) is re-checked against pid's current
// start time after opening the namespace file but before joining it, so that
// a pid recycled onto an unrelated process between publication and this call
// is detected and refused rather than silently joined.
util::expected<void, std::string>
namespace_join(pid_t pid, const std::string& ns,
               unsigned long long expected_start_time);

// Enter each of ns_names of the process pid, in the order given. See
// namespace_join() for expected_start_time.
util::expected<void, std::string>
namespaces_join(pid_t pid, const std::vector<std::string>& ns_names,
                unsigned long long expected_start_time);

} // namespace util
