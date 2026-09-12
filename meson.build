project('uenv', ['cpp'],
        default_options : [
          'cpp_std=c++20',
          'default_library=static',
          'werror=true',
          'warning_level=3',
        ],
        version: files('VERSION'),
        meson_version: '>=1.4')
version = meson.project_version()

uenv_slurm_plugin = get_option('slurm_plugin')
uenv_cli = get_option('cli')
uenv_squashfs_mount = get_option('squashfs_mount')
use_fuse = get_option('mount_backend') == 'fuse'

conf_data = configuration_data()

add_global_arguments('-Wno-missing-field-initializers', language : 'cpp')

# fixes warnings generated when building curl and sqlite3
# that started to occur with gcc 15.2.1
# Note that we only apply this to C, so it should not be applied to the C++ code
add_global_arguments('-Wno-discarded-qualifiers', language : 'c')

# configure the dependencies

# use meson wrap to provide the dependencies, because the 'default_library=static' option
# will be propogated to each dependency, for a statically linked executable.
cli11_dep = subproject('cli11', default_options: ['werror=false', 'warning_level=0']).get_variable('CLI11_dep')
fmt_dep   = subproject('fmt', default_options: ['werror=false', 'warning_level=0']).get_variable('fmt_dep')
json_dep  = subproject('nlohmann_json', default_options: ['werror=false', 'warning_level=0']).get_variable('nlohmann_json_dep')
spdlog_dep  = subproject('spdlog', default_options: ['werror=false', 'warning_level=0','std_format=disabled','external_fmt=enabled']).get_variable('spdlog_dep')
sqlite3_dep = subproject('sqlite3', default_options: ['werror=false', 'warning_level=0']).get_variable('sqlite3_dep')
toml_base = subproject('tomlplusplus', default_options: ['werror=false', 'warning_level=0', 'build_lib=false']).get_variable('tomlplusplus_dep')
toml_dep = declare_dependency(
  dependencies: toml_base,
  compile_args: ['-DTOML_EXCEPTIONS=0', '-DTOML_HEADER_ONLY=0'],
)
# zlib: needed for gzip handling in the native OCI registry client
# Built before curl so override_dependency('zlib') is in place for when curl is configured
zlib_dep = subproject('zlib', default_options: ['warning_level=0', 'werror=false', 'default_library=static']).get_variable('zlib_dep')
# openssl: the TLS backend for curl, and the SHA-256 implementation used by
# src/util/sha.cpp - libcrypto's is an order of magnitude faster than a portable
# C one, which matters because hashing bounds image push and pull.
# Configured before curl so override_dependency('openssl') is in place for when
# curl is configured; curl consumes it by name, we take libcrypto by hand.
# Unlike every other subproject this one is not a meson build - it drives
# OpenSSL's own Configure + make (see subprojects/packagefiles/openssl/), so
# warning_level/werror/default_library would have nothing to apply to.
libcrypto_dep = subproject('openssl').get_variable('libcrypto_dep')
# By default curl sets feature default to 'auto', picking up any dependencies
# it finds installed on the system.
# We want to only link against static dependencies provided by meson, so
# pin all features exactly.
subproject('curl',
        default_options: [
            'werror=false', 'warning_level=0',
            'tests=disabled', 'unittests=disabled', 'tool=disabled',
            'libuv=disabled', # only used by curl's own tests, and only in debug builds
            # TLS, via the vendored static openssl configured above
            'ssl=enabled', 'openssl=enabled',
            # uenv relies on the following
            #   ipv6      - registries that only publish a AAAA record
            #   asynchdns - threaded resolver, so the connect/total timeouts can
            #               interrupt a hung DNS lookup
            #   libz      - Content-Encoding: gzip, via the vendored static zlib
            'ipv6=enabled', 'asynchdns=enabled', 'libz=enabled',
            # everything else off. uenv speaks HTTP/1.1 over http(s) and nothing
            # else - see CURLOPT_PROTOCOLS_STR in src/util/curl.cpp - and
            # authenticates with Basic plus its own Authorization: Bearer header,
            # so none of the protocol, auth or name-resolution extras below are
            # reachable at runtime. They can be turned back on individually if a
            # feature ever needs one.
            'http2=disabled', 'brotli=disabled', 'zstd=disabled',
            'psl=disabled', 'idn=disabled', 'gsasl=disabled',
            'gss-api=disabled', 'ntlm=disabled', 'tls-srp=disabled',
            'alt-svc=disabled', 'hsts=disabled', 'unixsockets=disabled',
            'ssh=disabled', 'ldap=disabled', 'ldaps=disabled', 'rtmp=disabled',
            'dict=disabled', 'ftp=disabled', 'gopher=disabled', 'imap=disabled',
            'mqtt=disabled', 'pop3=disabled', 'rtsp=disabled', 'smb=disabled',
            'smtp=disabled', 'telnet=disabled', 'tftp=disabled',
        ])
curl_dep = dependency('libcurl', required: true)
# libarchive: used by the native OCI registry client to pack/unpack directories
libarchive_dep = subproject('libarchive',
        default_options: [
            'warning_level=0', 'werror=false', 'default_library=static',
            'progs=[]', 'tests=disabled', 'regex=disabled',
            'zlib=enabled',
            'bz2lib=disabled', 'libb2=disabled', 'lz4=disabled', 'zstd=disabled',
            'lzma=disabled', 'lzo2=disabled', 'iconv=disabled', 'cng=disabled',
            'openssl=disabled', 'xml2=disabled', 'expat=disabled',
            'xattr=disabled', 'acl=disabled',
        ]).get_variable('libarchive_dep')
barkeep_dep = declare_dependency(
        include_directories: 'extern/barkeep',
        compile_args: ['-DBARKEEP_ENABLE_FMT_FORMAT'],
)

libmount_dep = dependency('mount')

if use_fuse
    # zstd is configured first so squashfuse's dependency('libzstd') resolves to the wrapdb subproject
    subproject('zstd', default_options: [
        'warning_level=0', 'werror=false', 'default_library=static',
        'bin_programs=false', 'bin_tests=false', 'bin_contrib=false',
        'zlib=disabled', 'lzma=disabled', 'lz4=disabled',
    ])

    # squashfuse depends on zstd and zlib wrap packages
    # it is built from source as a static library using a custom meson configuration
    squashfuse_dep = subproject('squashfuse',
        default_options: ['warning_level=0', 'werror=false'],
    ).get_variable('squashfuse_ll_dep')

    # fuse3 is a dynamic dependency.
    fuse_dep = dependency('fuse3', required: true)
endif

# the lib dependency is all of the common funtionality shared between the CLI
# and the slurm plugin.
lib_uenv_src = [
        'src/oci/auth.cpp',
        'src/oci/client.cpp',
        'src/oci/digest.cpp',
        'src/oci/manifest.cpp',
        'src/oci/parse.cpp',
        'src/oci/pull.cpp',
        'src/oci/push.cpp',
        'src/oci/reference.cpp',
        'src/oci/tag.cpp',
        'src/oci/types.cpp',
        'src/oci/util.cpp',
        'src/uenv/elastic.cpp',
        'src/uenv/env.cpp',
        'src/uenv/join_context.cpp',
        'src/uenv/log.cpp',
        'src/uenv/meta.cpp',
        'src/uenv/mount.cpp',
        'src/uenv/parse.cpp',
        'src/uenv/print.cpp',
        'src/uenv/repository.cpp',
        'src/uenv/settings.cpp',
        'src/uenv/telemetry.cpp',
        'src/uenv/uenv.cpp',
        'src/util/archive.cpp',
        'src/util/cgroup.cpp',
        'src/util/color.cpp',
        'src/util/curl.cpp',
        'src/util/envvars.cpp',
        'src/util/fs.cpp',
        'src/util/lex.cpp',
        'src/util/lustre.cpp',
        'src/util/parse.cpp',
        'src/util/proc_barrier.cpp',
        'src/util/ready_fork.cpp',
        'src/util/semver.cpp',
        'src/util/setns.cpp',
        'src/util/sha.cpp',
        'src/util/shell.cpp',
        'src/util/signal.cpp',
        'src/util/strings.cpp',
        'src/util/subprocess.cpp',
        'src/util/tasks_per_node.cpp',
        'src/util/toml.cpp',
        'src/util/url.cpp',
        'src/site/site.cpp',
]

if use_fuse
    lib_uenv_src += ['src/uenv/mount_rootless.cpp']
else
    lib_uenv_src += ['src/uenv/mount_kernel.cpp']
endif

lib_uenv_inc = include_directories('src')

lib_uenv_deps = [
    barkeep_dep,
    curl_dep,
    fmt_dep,
    json_dep,
    libarchive_dep,
    libcrypto_dep,
    spdlog_dep,
    sqlite3_dep,
    toml_dep,
    zlib_dep,
]
if use_fuse
    lib_uenv_deps += [squashfuse_dep, fuse_dep]
else
    lib_uenv_deps += [libmount_dep]
endif

lib_uenv = static_library(
    'uenv',
    lib_uenv_src,
    dependencies: lib_uenv_deps,
    include_directories: lib_uenv_inc,
)

uenv_dep = declare_dependency(
    link_with: lib_uenv,
    dependencies: lib_uenv_deps,
    include_directories: lib_uenv_inc
)

subdir('src/uenv')

# the uenv executable
if uenv_cli
    cli_src = [
            'src/cli/add_remove.cpp',
            'src/cli/config.cpp',
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

    xxd = find_program('xxd', required : true)
    completion_bash_dep = custom_target(
        'completion-bash-dep',
        input: 'src/cli/completion.bash',
        output: 'completion.bash.h',
        command: [xxd, '-i', '-n', 'completion_bash_main', '@INPUT@', '@OUTPUT@'])

    cli = executable('uenv',
            sources: cli_src + completion_bash_dep,
            dependencies: [uenv_dep, sqlite3_dep, fmt_dep, spdlog_dep, cli11_dep, barkeep_dep],
            c_args: [
              '-DVERSION="@0@"'.format(version),
            ],
            install: true)

    meson.add_install_script('./meson-scripts/install-bash-completion.sh')
endif

if uenv_slurm_plugin
    slurm_version = get_option('slurm_version')
    subdir('src/slurm')
endif

if uenv_squashfs_mount
    if use_fuse
        squashfs_mount_src = ['src/squashfs-mount/squashfs-mount-fuse.cpp']
        squashfs_mount_install_mode = ['rwxr-xr-x', false, false]
    else
        squashfs_mount_src = ['src/squashfs-mount/squashfs-mount-kernel.cpp']
        squashfs_mount_install_mode = ['rwsr-xr-x', 'root', 'root']
    endif
    squashfs_mount = executable('squashfs-mount',
                                sources: squashfs_mount_src,
                                dependencies: [uenv_dep, cli11_dep],
                                cpp_args: ['-DVERSION="@0@"'.format(version)],
                                install_mode: squashfs_mount_install_mode,
                                install: true)
endif

if get_option('tests').enabled()
    catch_dep = subproject('catch2', default_options: ['werror=false', 'warning_level=0', 'tests=false']).get_variable('catch2_with_main_dep')
    subdir('test')
endif
