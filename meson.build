project('uenv', ['cpp'],
        default_options : [
          'cpp_std=c++17',
          'default_library=static',
        ],
        version: files('VERSION'),
        meson_version: '>=0.57')
version = meson.project_version()

#default_mount_point = get_option('default_mount_point')

conf_data = configuration_data()

# configure the dependencies
#
# use meson wrap to provide the dependencies, because the 'default_library=static' option
# will be propogated to each dependency, for a statically linked executable.
catch_dep = subproject('catch2', default_options: 'werror=false').get_variable('catch2_with_main_dep')
cli11_dep = subproject('cli11', default_options: 'werror=false').get_variable('CLI11_dep')
fmt_dep   = subproject('fmt', default_options: 'werror=false').get_variable('fmt_dep')
json_dep = subproject('nlohmann_json', default_options: 'werror=false').get_variable('nlohmann_json_dep')
spdlog_dep   = subproject('spdlog', default_options: 'werror=false').get_variable('spdlog_dep')
sqlite3_dep = subproject('sqlite3', default_options: 'werror=false').get_variable('sqlite3_dep')

# the lib dependency is all of the common funtionality shared between the CLI
# and the slurm plugin.
lib_src = [
        'src/uenv/env.cpp',
        'src/uenv/envvars.cpp',
        'src/uenv/lex.cpp',
        'src/uenv/log.cpp',
        'src/uenv/meta.cpp',
        'src/uenv/parse.cpp',
        'src/util/strings.cpp',
        'src/util/shell.cpp',
        'src/uenv/uenv.cpp',
]
lib_inc = include_directories('src')

lib_dep = declare_dependency(
        sources: lib_src,
        dependencies: [sqlite3_dep, fmt_dep, spdlog_dep, json_dep],
        include_directories: lib_inc
)

# the uenv executable
uenv_src = [
        'src/cli/env.cpp',
        'src/cli/start.cpp',
        'src/cli/uenv.cpp',
]
uenv_dep = [sqlite3_dep]

uenv = executable('uenv',
        sources: uenv_src,
        dependencies: [uenv_dep, lib_dep, fmt_dep, spdlog_dep, cli11_dep],
        c_args: ['-DVERSION="@0@"'.format(version)],
        install: true)

unit_src = [
        'test/unit/env.cpp',
        'test/unit/envvars.cpp',
        'test/unit/parse.cpp',
        'test/unit/lex.cpp',
        'test/unit/main.cpp',
]

unit = executable('unit',
        sources: unit_src,
        dependencies: [catch_dep, lib_dep],
        build_by_default: true,
        install: false)

