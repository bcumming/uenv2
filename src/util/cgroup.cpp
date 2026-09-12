#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <util/cgroup.h>

namespace util {

std::optional<std::string> current_cgroup() {
    std::ifstream in{"/proc/self/cgroup"};
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto contents = buffer.str();
    const auto end = contents.find_last_not_of(" \t\n\r");
    if (end == std::string::npos) {
        return {};
    }
    contents.resize(end + 1);
    return contents;
}

bool cgroup_is_slurm_managed(std::string_view cgroup) {
    // leading '/' avoids matching a component that merely ends in these
    // substrings, e.g. "myjob_17" or "myslurmstepd.scope".
    if (cgroup.find("/job_") != std::string_view::npos) {
        return true;
    }
    // some cgroup v2 deployments name job/step with an opaque token instead
    // of "job_<id>/step_<n>"; the scope is the stable part.
    return cgroup.find("/slurmstepd.scope/") != std::string_view::npos;
}

} // namespace util
