#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include <barkeep/barkeep.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <uenv/oras.h>
#include <uenv/uenv.h>
#include <util/color.h>
#include <util/fs.h>
#include <util/signal.h>
#include <util/subprocess.h>

namespace uenv {
namespace oras {

using opt_creds = std::optional<credentials>;

// stores the "result" of calling the oras cli tool as an external process.
struct oras_output {
    // return code of the oras call
    int returncode = -1;

    // the full captured stdout output
    std::string stdout;

    // the full captured stderr output
    std::string stderr;
};

constexpr auto generic_error_message =
    "unknown error - rerun with the -vvv flag and send an error report to the "
    "CSCS service desk.";

error generic_error(std::string err) {
    return {-1, std::move(err), generic_error_message};
}

// Convert oras_output to a uenv::oras::error.
// oras returns 1 for all errors that it bothers signalling (from what we
// observe - there is no documentation). sometimes it returns 0, and you have to
// parse stderr to double check.
// Hence, parse stderr & stdout for hints about
// what went wrong to generate the user-friendly message
error create_error(const oras_output& result) {
    std::string message = generic_error_message;

    auto contains = [](std::string_view string,
                       std::string_view contents) -> bool {
        return string.find(contents) != std::string_view::npos;
    };
    std::string_view err = result.stderr;
    if (contains(err, "403") && contains(err, "Forbidden")) {
        return {
            403, result.stderr,
            "Invalid credentials were provided, or you may not have permission "
            "to perform the requested action.\nTry using the --token flag if "
            "you are trying to access restricted software.\nCSCS staff can "
            "configure oras to use their credentials."};
    }
    if (contains(err, "Token failed verification: parse") &&
        contains(err, "Error response from registry")) {
        return {
            403, result.stderr,
            "The token failed parsing. It may be invalid, the wrong token,\n"
            "or need to be regenerated."};
    }
    if (contains(err, "Wrong username was used") &&
        contains(err, "Error response from registry")) {
        return {403, result.stderr,
                "Invalid username was provided. Check the --username flag."};
    }
    if (contains(err, "unauthorized") &&
        contains(err, "Error response from registry")) {
        return {403, result.stderr,
                fmt::format("no authorization: provide valid --token and "
                            "--username arguments.",
                            err)};
    }
    if (result.returncode == 0) {
        return {0, {}, {}};
    }
    return {result.returncode, result.stderr, std::move(message)};
};

std::vector<std::string>
redact_arguments(const std::vector<std::string>& args) {
    std::vector<std::string> redacted_args{};
    bool redact_next = false;
    for (auto arg : args) {
        if (redact_next) {
            redacted_args.push_back(std::string(arg.size(), 'X'));
            redact_next = false;
        } else if (arg.find("password") != std::string::npos) {
            if (auto eqpos = arg.find("="); eqpos != std::string::npos) {
                redacted_args.push_back(
                    std::string(arg.begin(), arg.begin() + eqpos + 1) +
                    std::string(arg.size() - eqpos, 'X'));
            } else {
                redacted_args.push_back(arg);
                redact_next = true;
            }
        } else {
            redacted_args.push_back(arg);
        }
    }

    return redacted_args;
}

oras_output
run_oras(std::vector<std::string> args,
         std::optional<std::filesystem::path> runpath = std::nullopt) {
    auto oras = util::oras_path();
    if (!oras) {
        spdlog::error("no oras executable found");
        return {.returncode = -1};
    }

    args.insert(args.begin(), oras->string());
    spdlog::trace("run_oras: {}", fmt::join(redact_arguments(args), " "));
    auto proc = util::run(args, runpath);

    auto result = proc->wait();

    return {.returncode = result,
            .stdout = proc->out.string(),
            .stderr = proc->err.string()};
}

util::expected<util::subprocess, std::string>
run_oras_async(std::vector<std::string> args,
               std::optional<std::filesystem::path> runpath = std::nullopt) {
    auto oras = util::oras_path();
    if (!oras) {
        return util::unexpected{"no oras executable found"};
    }

    args.insert(args.begin(), oras->string());
    spdlog::trace("run_oras_async: {}", fmt::join(redact_arguments(args), " "));

    return util::run(args, runpath);
}

util::expected<std::vector<std::string>, error>
discover(const std::string& registry, const std::string& nspace,
         const uenv_record& uenv, const opt_creds token) {
    auto address =
        fmt::format("{}/{}/{}/{}/{}/{}:{}", registry, nspace, uenv.system,
                    uenv.uarch, uenv.name, uenv.version, uenv.tag);

    std::vector<std::string> args = {"discover",        "--format",  "json",
                                     "--artifact-type", "uenv/meta", address};
    if (token) {
        args.push_back("--password");
        args.push_back(token->token);
        args.push_back("--username");
        args.push_back(token->username);
    }
    auto result = run_oras(args);

    if (result.returncode) {
        spdlog::error("oras discover returncode={} stderr='{}'",
                      result.returncode, result.stderr);
        return util::unexpected{create_error(result)};
    }

    std::vector<std::string> manifests;
    using json = nlohmann::json;
    try {
        const auto raw = json::parse(result.stdout);
        for (const auto& j : raw["manifests"]) {
            manifests.push_back(j["digest"]);
        }
    } catch (std::exception& e) {
        spdlog::error("unable to parse oras discover json: {}", e.what());
        return util::unexpected(generic_error(e.what()));
    }

    return manifests;
}

util::expected<void, error>
pull_digest(const std::string& registry, const std::string& nspace,
            const uenv_record& uenv, const std::string& digest,
            const std::filesystem::path& destination, const opt_creds token) {
    auto address =
        fmt::format("{}/{}/{}/{}/{}/{}@{}", registry, nspace, uenv.system,
                    uenv.uarch, uenv.name, uenv.version, digest);

    spdlog::debug("oras::pull_digest: {}", address);

    std::vector<std::string> args{"pull", "--output", destination.string(),
                                  address};
    if (token) {
        args.push_back("--password");
        args.push_back(token->token);
        args.push_back("--username");
        args.push_back(token->username);
    }
    auto proc = run_oras(args);

    if (proc.returncode) {
        spdlog::error("unable to pull digest with oras: {}", proc.stderr);
        return util::unexpected{create_error(proc)};
    }

    return {};
}

util::expected<void, error> pull_tag(const std::string& registry,
                                     const std::string& nspace,
                                     const uenv_record& uenv,
                                     const std::filesystem::path& destination,
                                     const opt_creds token) {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;
    namespace bk = barkeep;

    auto address =
        fmt::format("{}/{}/{}/{}/{}/{}:{}", registry, nspace, uenv.system,
                    uenv.uarch, uenv.name, uenv.version, uenv.tag);

    spdlog::debug("oras::pull_tag: {}", address);
    std::vector<std::string> args{"pull",     "--concurrency",      "10",
                                  "--output", destination.string(), address};
    if (token) {
        args.push_back("--password");
        args.push_back(token->token);
        args.push_back("--username");
        args.push_back(token->username);
    }
    auto proc = run_oras_async(args);

    if (!proc) {
        spdlog::error("unable to pull tag with oras: {}", proc.error());
        return util::unexpected{generic_error(proc.error())};
    }

    const fs::path& sqfs = destination / "store.squashfs";

    std::size_t downloaded_mb{0u};
    // force rounding up, so that total_mb is never zero
    std::size_t total_mb{(uenv.size_byte + (1024 * 1024 - 1)) / (1024 * 1024)};
    spdlog::info("byte {} MB {}", uenv.size_byte, total_mb);
    auto bar = bk::ProgressBar(
        &downloaded_mb,
        {
            .total = total_mb,
            .message = fmt::format("pulling {}", uenv.id.string()),
            .speed = 1.,
            .speed_unit = "MB/s",
            .style = color::use_color() ? bk::ProgressBarStyle::Rich
                                        : bk::ProgressBarStyle::Bars,
            .no_tty = !isatty(fileno(stdout)),
        });

    util::set_signal_catcher();
    while (!proc->finished()) {
        std::this_thread::sleep_for(500ms);
        // handle a signal, usually SIGTERM or SIGINT
        if (util::signal_raised()) {
            spdlog::error("signal raised - interrupting download");
            proc->kill();
            throw util::signal_exception(util::last_signal_raised());
        }
        if (fs::is_regular_file(sqfs)) {
            auto downloaded_bytes = fs::file_size(sqfs);
            downloaded_mb = downloaded_bytes / (1024 * 1024);
        }
    }
    downloaded_mb = total_mb;

    if (proc->rvalue()) {
        spdlog::error("unable to pull tag with oras: {}", proc->err.string());
        return util::unexpected{create_error({.returncode = proc->rvalue(),
                                              .stdout = proc->out.string(),
                                              .stderr = proc->err.string()})};
    }

    return {};
}

util::expected<void, error> push_tag(const std::string& registry,
                                     const std::string& nspace,
                                     const uenv_label& label,
                                     const std::filesystem::path& source,
                                     const std::optional<credentials> token) {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;
    namespace bk = barkeep;

    // Format the registry address with the appropriate path structure
    auto address =
        fmt::format("{}/{}/{}/{}/{}/{}:{}", registry, nspace, *label.system,
                    *label.uarch, *label.name, *label.version, *label.tag);

    spdlog::debug("oras::push_tag: {}", address);

    // Prepare the arguments for the push command
    std::vector<std::string> args{"push", "--concurrency", "10"};

    if (token) {
        args.push_back("--password");
        args.push_back(token->token);
        args.push_back("--username");
        args.push_back(token->username);
    }

    // Add artifact type and annotations
    args.push_back("--artifact-type");
    args.push_back("application/x-squashfs");

    // Add the destination address
    args.push_back(address);
    // Add the file to push - note that we strip the absolute and relative path
    args.push_back("./" + source.filename().string());

    // Run the oras command asynchronously
    // Note: oras must be called in the path of the file
    auto proc = run_oras_async(args, source.parent_path());

    if (!proc) {
        spdlog::error("unable to push tag with oras: {}", proc.error());
        return util::unexpected{generic_error(proc.error())};
    }

    // Create a spinner to show upload progress
    auto spinner = bk::Animation({
        .message = fmt::format("pushing {} to registry",
                               fs::path(source).filename().string()),
        .style = bk::Ellipsis,
        .no_tty = !isatty(fileno(stdout)),
    });

    // Handle signals during upload (e.g., Ctrl+C)
    util::set_signal_catcher();

    // Loop until the upload process completes
    while (!proc->finished()) {
        std::this_thread::sleep_for(100ms);

        // Handle interruption signals
        if (util::signal_raised()) {
            spdlog::error("signal raised - interrupting upload");
            proc->kill();
            throw util::signal_exception(util::last_signal_raised());
        }
    }

    spinner->done();

    // Check if the upload was successful
    auto err = create_error({.returncode = proc->rvalue(),
                             .stdout = proc->out.string(),
                             .stderr = proc->err.string()});
    if (err.returncode) {
        spdlog::error("unable to push tag with oras: {}", proc->err.string());
        return util::unexpected{err};
    }

    return {};
}

util::expected<void, error> push_meta(const std::string& registry,
                                      const std::string& nspace,
                                      const uenv_label& label,
                                      const std::filesystem::path& meta_path,
                                      const std::optional<credentials> token) {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;
    namespace bk = barkeep;

    // Check if the meta path exists and is a directory
    if (!fs::exists(meta_path) || !fs::is_directory(meta_path)) {
        spdlog::error(
            "metadata directory {} does not exist or is not a directory",
            meta_path.string());
        return util::unexpected{error(
            1, "metadata directory not found",
            fmt::format(
                "metadata directory {} does not exist or is not a directory",
                meta_path.string()))};
    }

    // Format the registry address with the appropriate path structure
    auto address =
        fmt::format("{}/{}/{}/{}/{}/{}:{}", registry, nspace, *label.system,
                    *label.uarch, *label.name, *label.version, *label.tag);

    spdlog::debug("oras::push_meta: {}", address);

    // Prepare the arguments for the attach command
    std::vector<std::string> args{"attach"};

    if (token) {
        args.push_back("--password");
        args.push_back(token->token);
        args.push_back("--username");
        args.push_back(token->username);
    }

    // Add artifact type and annotations for metadata
    args.push_back("--artifact-type");
    args.push_back("uenv/meta");

    // Add destination address
    args.push_back(address);
    // Add the meta data path - strip relative/absolute path from the path
    args.push_back("./" + meta_path.filename().string());

    // Run the oras command asynchronously
    // Note: oras must be called in the path of the file
    auto proc = run_oras_async(args, meta_path.parent_path());

    if (!proc) {
        spdlog::error("unable to push metadata with oras: {}", proc.error());
        return util::unexpected{generic_error(proc.error())};
    }

    // Create a spinner to show upload progress
    auto spinner = bk::Animation({
        .message = fmt::format("pushing metadata to registry"),
        .style = bk::Ellipsis,
        .no_tty = !isatty(fileno(stdout)),
    });

    // Handle signals during upload (e.g., Ctrl+C)
    util::set_signal_catcher();

    // Loop until the upload process completes
    while (!proc->finished()) {
        std::this_thread::sleep_for(100ms);

        // Handle interruption signals
        if (util::signal_raised()) {
            spdlog::error("signal raised - interrupting metadata upload");
            proc->kill();
            throw util::signal_exception(util::last_signal_raised());
        }
    }

    spinner->done();

    // Check if the upload was successful
    auto err = create_error({.returncode = proc->rvalue(),
                             .stdout = proc->out.string(),
                             .stderr = proc->err.string()});
    if (err.returncode) {
        spdlog::error("unable to metadata with oras: {}", proc->err.string());
        return util::unexpected{err};
    }

    return {};
}

util::expected<void, error>
copy(const std::string& registry, const std::string& src_nspace,
     const uenv_record& src_uenv, const std::string& dst_nspace,
     const uenv_record& dst_uenv, const std::optional<credentials> token) {

    auto address = [&registry](auto& nspace, auto& record) -> std::string {
        return fmt::format("{}/{}/{}/{}/{}/{}:{}", registry, nspace,
                           record.system, record.uarch, record.name,
                           record.version, record.tag);
    };
    const auto src_url = address(src_nspace, src_uenv);
    const auto dst_url = address(dst_nspace, dst_uenv);

    std::vector<std::string> args = {"cp",          "--concurrency", "10",
                                     "--recursive", src_url,         dst_url};
    if (token) {
        args.push_back(fmt::format("--from-password={}", token->token));
        args.push_back(fmt::format("--from-username={}", token->username));
        args.push_back(fmt::format("--to-password={}", token->token));
        args.push_back(fmt::format("--to-username={}", token->username));
    }
    auto result = run_oras(args);

    if (result.returncode) {
        spdlog::error("oras cp {}: {}", result.returncode, result.stderr);
        return util::unexpected{create_error(result)};
    }

    return {};
}

util::expected<void, error> login(const std::string& url,
                                  const credentials& credentials,
                                  const std::filesystem::path& path) {
    std::vector<std::string> args = {
        "login", fmt::format("--registry-config={}", path.string()),
        fmt::format("--username={}", credentials.username),
        fmt::format("--password={}", credentials.token), url};

    auto result = run_oras(args);

    if (result.returncode) {
        spdlog::error("oras login {}: {}", result.returncode, result.stderr);
        return util::unexpected{create_error(result)};
    }

    return {};
}

/*
util::expected<std::string, error>
fetch_manifest(const std::string& url,
               const std::optional<std::filesystem::path>& credentials) {

    std::vector<std::string> args = {"manifest", "fetch", url};
}
*/

} // namespace oras
} // namespace uenv
