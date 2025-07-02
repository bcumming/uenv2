project('uenv', ['cpp'],
        default_options : [
          'cpp_std=c++20',
          'default_library=static',
          'werror=true',
          'warning_level=3',
        ],
        version: files('VERSION'),
        meson_version: '>=1.3')
version = meson.project_version()

oras_version = get_option('oras_version')

uenv_slurm_plugin = get_option('slurm_plugin')
uenv_cli = get_option('cli')

conf_data = configuration_data()

add_global_arguments('-Wno-missing-field-initializers', language : 'cpp')

# configure the dependencies

# use meson wrap to provide the dependencies, because the 'default_library=static' option
# will be propogated to each dependency, for a statically linked executable.
catch_dep = subproject('catch2', default_options: ['werror=false', 'warning_level=0', 'tests=false']).get_variable('catch2_with_main_dep')
cli11_dep = subproject('cli11', default_options: ['werror=false', 'warning_level=0']).get_variable('CLI11_dep')
fmt_dep   = subproject('fmt', default_options: ['werror=false', 'warning_level=0']).get_variable('fmt_dep')
json_dep = subproject('nlohmann_json', default_options: ['werror=false', 'warning_level=0']).get_variable('nlohmann_json_dep')
spdlog_dep   = subproject('spdlog', default_options: ['werror=false', 'warning_level=0','std_format=disabled','external_fmt=enabled']).get_variable('spdlog_dep')
sqlite3_dep = subproject('sqlite3', default_options: ['werror=false', 'warning_level=0']).get_variable('sqlite3_dep')
subproject('curl', default_options: ['werror=false', 'warning_level=0', 'tests=disabled', 'unittests=disabled', 'tool=disabled'])

curl_dep = dependency('libcurl', required: true)
barkeep_dep = declare_dependency(
        include_directories: 'extern/barkeep',
        compile_args: ['-DBARKEEP_ENABLE_FMT_FORMAT'],
)

# the lib dependency is all of the common funtionality shared between the CLI
# and the slurm plugin.
lib_src = [
        'src/site/site.cpp',
        'src/uenv/env.cpp',
        'src/uenv/log.cpp',
        'src/uenv/meta.cpp',
        'src/uenv/oras.cpp',
        'src/uenv/parse.cpp',
        'src/uenv/print.cpp',
        'src/uenv/registry.cpp',
        'src/uenv/repository.cpp',
        'src/uenv/settings.cpp',
        'src/uenv/uenv.cpp',
        'src/util/color.cpp',
        'src/util/curl.cpp',
        'src/util/envvars.cpp',
        'src/util/fs.cpp',
        'src/util/lex.cpp',
        'src/util/shell.cpp',
        'src/util/signal.cpp',
        'src/util/strings.cpp',
        'src/util/subprocess.cpp',
]
lib_inc = include_directories('src')

lib_uenv = static_library(
        'libuenv',
        lib_src,
        include_directories: lib_inc,
        dependencies: [curl_dep, sqlite3_dep, fmt_dep, spdlog_dep, json_dep, barkeep_dep],
)

uenv_dep = declare_dependency(
        link_with: lib_uenv,
        dependencies: [curl_dep, sqlite3_dep, fmt_dep, spdlog_dep, json_dep, barkeep_dep],
        include_directories: lib_inc
)

subdir('src/uenv')

# the uenv executable
if uenv_cli
    cli_src = [
            'src/cli/add_remove.cpp',
            'src/cli/copy.cpp',
            'src/cli/delete.cpp',
            'src/cli/find.cpp',
            'src/cli/help.cpp',
            'src/cli/image.cpp',
            'src/cli/inspect.cpp',
            'src/cli/ls.cpp',
            'src/cli/pull.cpp',
            'src/cli/push.cpp',
            'src/cli/repo.cpp',
            'src/cli/run.cpp',
            'src/cli/start.cpp',
            'src/cli/status.cpp',
            'src/cli/uenv.cpp',
            'src/cli/util.cpp',
            'src/cli/build.cpp',
            'src/cli/completion.cpp',
    ]

    cli = executable('uenv',
            sources: cli_src,
            dependencies: [uenv_dep, sqlite3_dep, fmt_dep, spdlog_dep, cli11_dep, barkeep_dep],
            c_args: [
              '-DVERSION="@0@"'.format(version),
            ],
            install: true)

    arch = 'amd64'
    if host_machine.cpu_family() == 'aarch64'
        arch = 'arm64'
    endif
    oras_url = 'https://github.com/oras-project/oras/releases/download/v@0@/oras_@0@_linux_@1@.tar.gz'.format(oras_version, arch)

    oras_download = custom_target(
        'oras-download',
        output: 'oras.tar.gz',
        command: ['wget', '--quiet', oras_url, '-O', '@OUTPUT@'],
        install: false,
    )

    oras_extract = custom_target(
        'oras-extract',
        input: oras_download,
        output: 'oras',
        command: ['tar', 'xf', '@INPUT@', 'oras'],
        install: true,
        install_dir: get_option('libexecdir'),
    )

    meson.add_install_script('./meson-scripts/install-bash-completion.sh')
endif

if uenv_slurm_plugin
    subdir('src/slurm')
endif

if get_option('tests').enabled()
    subdir('test')
endif
