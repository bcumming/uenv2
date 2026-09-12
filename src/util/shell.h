#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <util/envvars.h>
#include <util/expected.h>

namespace util {

/// returns the path of the current shell
util::expected<std::filesystem::path, std::string>
current_shell(const envvars::state&);

/// replicates the process used by execvp/which to find an executable
std::optional<std::filesystem::path> which(std::string const& name,
                                           std::string const& PATH);

/// get current working directory
std::optional<std::filesystem::path> cwd();

// return value of exec(), which is a wrapper around execvp/execvpe
// it is always an error when exec returns, because success implies
// that the current process has had its stack erased by exec.
struct exec_error {
    int rcode;
    std::string message;

    exec_error() = delete;
    explicit exec_error(int r, std::string m)
        : rcode(r), message(std::move(m)) {
    }
};

/// execve
exec_error exec(const std::vector<std::string>& argv,
                char* const envp[] = nullptr);

} // namespace util
