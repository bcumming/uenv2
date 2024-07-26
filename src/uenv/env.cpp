#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <uenv/env.h>
#include <uenv/parse.h>

namespace uenv {

util::expected<env, std::string>
concretise_env(const std::string& uenv_args,
               std::optional<std::string> view_args) {
    namespace fs = std::filesystem;

    // parse the uenv description that was provided as a command line argument.
    // the command line argument is a comma-separated list of uenvs, where each
    // uenc is either
    // - the path of a squashfs image; or
    // - a uenv description of the form name/version:tag
    // with an optional mount point.
    const auto uenv_descriptions = uenv::parse_uenv_args(uenv_args);
    if (!uenv_descriptions) {
        return util::unexpected(
            fmt::format("unable to read the uenv argument\n  {}",
                        uenv_descriptions.error().msg));
    }

    // concretise the uenv descriptions by looking for the squashfs file, or
    // looking up the uenv descrition in a registry.
    // after this loop, we have fully validated list of uenvs, mount points and
    // meta data (if they have meta data).

    // the following are for assigning default mount points when no explicit
    // mount point is provided.
    std::vector<std::optional<std::string>> default_mounts{
        "/user-environment", "/user-tools", {}};
    auto default_mount = default_mounts.begin();

    std::unordered_map<std::string, uenv::concrete_uenv> uenvs;
    for (auto& desc : *uenv_descriptions) {
        // determine the mount point of the uenv, then validate that it exists.
        fs::path mount;
        if (auto m = desc.mount()) {
            mount = *m;
            // once an explicit mount point has been set defaults are no longer
            // applied. set default mount to the last entry, which is a nullopt
            // std::optional.
            default_mount = std::prev(default_mounts.end());
        } else {
            // no mount point was provided for this uenv, so use the default if
            // it is available.
            if (*default_mount) {
                mount = **default_mount;
                ++default_mount;
            } else {
                return util::unexpected(
                    fmt::format("no mount point provided for {}", desc));
            }
        }
        spdlog::info("{} will be mounted at {}", desc, mount);

        // check that the mount point exists
        if (!fs::exists(mount)) {
            return util::unexpected(
                fmt::format("the mount point '{}' does not exist", mount));
        }

        if (desc.label()) {
            return util::unexpected(
                fmt::format("support for mounting uenv from labels is not "
                            "supported yet: '{}'",
                            *desc.label()));
        }

        // get the sqfs_path
        // desc.filename - check that it exists
        // desc.label - perform database lookup (TODO)

        // TODO parameterise whether an absolute path is a hard requirement (for
        // the SLURM plugin it is)
        auto sqfs_path = fs::path(*desc.filename());
        if (!fs::exists(sqfs_path)) {
            return util::unexpected(
                fmt::format("{} does not exist", sqfs_path));
        }
        sqfs_path = fs::absolute(sqfs_path);
        if (!fs::is_regular_file(sqfs_path)) {
            return util::unexpected(fmt::format("{} is not a file", sqfs_path));
        }

        // set the meta data path and env.json path if they exist
        const auto img_root_path = sqfs_path.parent_path();
        const auto meta_p = img_root_path / "meta";
        const auto env_meta_p = meta_p / "env.json";
        const std::optional<fs::path> meta_path =
            fs::is_directory(meta_p) ? meta_p : std::optional<fs::path>{};
        const std::optional<fs::path> env_meta_path =
            fs::is_regular_file(env_meta_p) ? env_meta_p
                                            : std::optional<fs::path>{};

        // if meta/env.json exists, parse the json therein
        std::string name;
        std::string description;
        std::unordered_map<std::string, concrete_view> views;
        if (env_meta_path) {
            if (const auto result = uenv::load_meta(*env_meta_path)) {
                name = std::move(result->name);
                description = std::move(result->description);
                views = std::move(result->views);
                spdlog::debug("loaded meta with name {}", name);
            } else {
                spdlog::error("error loading the uenv meta data in {}: {}",
                              *env_meta_path, result.error());
            }
        } else {
            spdlog::debug("the meta data file {} does not exist", meta_path);
            description = "";
            // generate a unique name for the uenv
            name = "anonymous";
            unsigned i = 1;
            while (uenvs.count(name)) {
                name = fmt::format("anonymous{}", i);
                ++i;
            }
        }

        uenvs[name] = concrete_uenv{name,      mount,       sqfs_path,
                                    meta_path, description, std::move(views)};
    }

    // A dictionary with view name as a key, and a list of uenv that provide
    // view with that name as the values
    // std::unordered_map<std::string, std::vector<std::string>> view2uenv;
    std::unordered_map<std::string, std::vector<std::string>> view2uenv;
    for (const auto& u : uenvs) {
        for (const auto& v : u.second.views) {
            if (!view2uenv.count(v.first)) {
                view2uenv[v.first] = {u.first};
            } else {
                view2uenv[v.first].push_back(u.first);
            }
        }
    }

    // step 2: parse the views
    //  - parse the CLI view description
    //  - look up view descriptions in the uenv meta data
    std::vector<qualified_view_description> views;
    if (view_args) {
        const auto view_descriptions = uenv::parse_view_args(*view_args);
        if (!view_descriptions) {
            return util::unexpected(fmt::format("invalid view description: {}",
                                                view_descriptions.error().msg));
        }

        for (auto& view : *view_descriptions) {
            // check whether the view name matches the name of any views
            // provided by uenv
            if (view2uenv.count(view.name)) {
                // a list of uenv that have a view with name v.name
                const auto& matching_uenvs = view2uenv[view.name];

                // handle the case where no uenv name was provided, e.g. develop
                if (!view.uenv) {
                    // it is ambiguous if more than one option is available
                    if (matching_uenvs.size() > 1) {
                        return util::unexpected("ambiguous view name");
                    }
                    views.push_back({matching_uenvs[0], view.name});
                }
                // handle the case where both uenv and view name are provided,
                // e.g. prgenv-gnu:develop
                else {
                    auto it = std::find_if(
                        matching_uenvs.begin(), matching_uenvs.end(),
                        [&n = *view.uenv](const std::string& uenv_name) {
                            return uenv_name == n;
                        });
                    // no uenv matches
                    if (it == matching_uenvs.end()) {
                        return util::unexpected("");
                    }
                    views.push_back({*it, view.name});
                }
            }
            // no view that matches the view is available
            else {
                return util::unexpected(
                    fmt::format("the requested view '{}' is not "
                                "provided by any of the uenv",
                                view.name));
            }
        }
    }

    return env{uenvs, views};
}

} // namespace uenv
