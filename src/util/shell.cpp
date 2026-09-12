#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <util/expected.h>
#include <util/fs.h>
#include <util/shell.h>
#include <util/strings.h>

namespace {

// path constructor can throw exceptions (yay!)
std::optional<std::filesystem::path> make_path(const std::string& name) {
    try {
        return std::filesystem::path{name};
    } catch (...) {
        return {};
    }
};

} // namespace

namespace util {

/// returns the path of the shell currently being used
util::expected<std::filesystem::path, std::string>
current_shell(const envvars::state& envvars) {
    auto env_shell = envvars.get("SHELL");

    if (!env_shell) {
        return util::unexpected("SHELL environment variable is not set");
    }

    const auto raw_path = std::filesystem::path(*env_shell);
    std::error_code ec;
    const auto can_path = std::filesystem::canonical(raw_path, ec);

    if (ec) {
        return util::unexpected(
            fmt::format("unable to determine canonical form of the shell '{}'",
                        raw_path.string()));
    }

    return can_path;
}

std::optional<std::filesystem::path> cwd() {
    char* p = ::getcwd(nullptr, 0);
    if (p == nullptr) {
        return std::nullopt;
    }

    try {
        auto r = std::filesystem::path(p);
        free(p);
        return r;
    } catch (...) {
        free(p);
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> which(std::string const& name,
                                           std::string const& PATH) {
    namespace fs = std::filesystem;

    auto is_executable = [](const fs::path& p) {
        return fs::exists(p) && fs::is_regular_file(p) &&
               access(p.c_str(), X_OK) == 0;
    };

    if (name.find('/') != std::string::npos) {
        const auto p = make_path(name);
        if (p && is_executable(*p)) {
            return fs::canonical(*p);
        }
        return {};
    }

    for (auto& path : split(PATH, ':', true)) {
        if (const auto root = make_path(path)) {
            const auto candidate = *root / name;
            if (is_executable(candidate)) {
                return fs::canonical(candidate);
            }
        }
    }

    return {};
}

exec_error exec(const std::vector<std::string>& args, char* const envp[]) {
    std::vector<char*> argv;

    // clean up temporary files before calling execve, because the descructor
    // that manages their deletion will not be deleted after the current process
    // has been replaced.
    util::clear_temp_dirs();

    // unsafe {
    for (auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    // WARNING: do not log all arguments to the command that is being executed
    //          because it might leak secrets: it is the responsibility of
    //          callers to log the call appropriately, removing any secrets
    //          before printing.
    int r;
    if (envp == nullptr) {
        spdlog::info("execvp({})", args[0]);
        r = execvp(argv[0], argv.data());
    } else {
        spdlog::info("execvpe({})", args[0]);
        r = execvpe(argv[0], argv.data(), envp);
    }
    // } // end unsafe

    std::string errmsg = fmt::format("unable to exec '{}': {} (errno={})",
                                     args[0], std::strerror(errno), errno);

    spdlog::error("{}", errmsg);

    return exec_error{r, std::move(errmsg)};
}

} // namespace util
