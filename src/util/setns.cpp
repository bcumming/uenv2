#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <util/expected.h>
#include <util/fs.h>
#include <util/setns.h>

namespace util {

// /proc/pid/stat's second field is "(comm)" -- the executable name, which can
// itself contain spaces and parentheses -- so the only safe way to find the
// start of the fields that follow it is to split after the *last* ')' on the
// line, per proc(5).
util::expected<unsigned long long, std::string> process_start_time(pid_t pid) {
    const auto path = fmt::format("/proc/{}/stat", pid);
    auto line = util::read_single_line_file(path);
    if (!line) {
        return util::unexpected(fmt::format("can't read {}", path));
    }

    const auto comm_end = line->rfind(')');
    if (comm_end == std::string::npos || comm_end + 2 > line->size()) {
        return util::unexpected(fmt::format("malformed {}", path));
    }

    // fields after comm, 1-indexed from proc(5): state(3) ppid(4) pgrp(5)
    // session(6) tty_nr(7) tpgid(8) flags(9) minflt(10) cminflt(11)
    // majflt(12) cmajflt(13) utime(14) stime(15) cutime(16) cstime(17)
    // priority(18) nice(19) num_threads(20) itrealvalue(21) starttime(22).
    std::istringstream rest(line->substr(comm_end + 2));
    std::string field;
    for (int n = 3; n < 22; ++n) {
        if (!(rest >> field)) {
            return util::unexpected(fmt::format("malformed {}", path));
        }
    }
    unsigned long long starttime;
    if (!(rest >> starttime)) {
        return util::unexpected(fmt::format("malformed {}", path));
    }
    return starttime;
}

// Join a specific namespace.
util::expected<void, std::string>
namespace_join(pid_t pid, const std::string& ns,
               unsigned long long expected_start_time) {
    std::string path;
    int fd;

    path = fmt::format("/proc/{}/ns/{}", pid, ns);
    fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT) {
            return util::unexpected(
                fmt::format("join: no PID {}: {} not found", pid, path));
        } else {
            return util::unexpected(fmt::format("join: can't open {}", path));
        }
    }

    // Guard against pid having been recycled onto an unrelated process
    // between whoever published it and this open() call: re-check its start
    // time now that we hold an fd on its namespace (which pins the
    // namespace, so from here on the fd is trustworthy regardless of what
    // happens to pid next) and refuse to join on a mismatch.
    auto now_start = process_start_time(pid);
    if (!now_start || *now_start != expected_start_time) {
        close(fd);
        if (!now_start) {
            return util::unexpected(
                fmt::format("join: could not verify identity of pid {}: {}",
                            pid, now_start.error()));
        }
        return util::unexpected(fmt::format(
            "join: pid {} was recycled before it could be joined (expected "
            "start time {}, found {})",
            pid, expected_start_time, *now_start));
    }

    // setns(2) seems to be involved in some kind of race with syslog(3).
    // Rarely, when configured with --enable-syslog, the call fails with
    // EINVAL. We never figured out a proper fix, so just retry a few times in
    // a loop. See issue https://github.com/hpc/charliecloud/issues/1270.
    for (int i = 1; setns(fd, 0) != 0; i++) {
        if (i >= 5) {
            return util::unexpected(
                fmt::format("can’t join {} namespace of pid {}", ns, pid));
        } else {
            spdlog::warn("can’t join {} namespace; trying again, {}", ns,
                         strerror(errno));
            sleep(1);
        }
    }
    return {};
}

// Join the existing namespaces containing process pid.
util::expected<void, std::string>
namespaces_join(pid_t pid, const std::vector<std::string>& ns_names,
                unsigned long long expected_start_time) {
    for (auto& ns : ns_names) {
        spdlog::trace("joining namespace {} of pid {}", ns, pid);
        auto r = namespace_join(pid, ns, expected_start_time);
        if (!r) {
            return r;
        }
    }
    return {};
}

} // namespace util
