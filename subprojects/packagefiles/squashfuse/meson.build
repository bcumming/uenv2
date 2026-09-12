# squashfuse, built as a native compile-time meson static library.
#
# squashfuse is autotools upstream, but - unlike OpenSSL, whose generated
# headers curl probes at configure time - nothing here needs a configure-time
# build. So instead of driving ./configure + make (the OpenSSL pattern), we
# compile the low-level library directly, the way wrapdb ports curl: zstd comes
# from the wrapdb zstd subproject, fuse3 from the system, config.h from
# config.h.in below.
#
# The source list is the union of squashfuse's three "convenience" archives
# (libsquashfuse_convenience + libsquashfuse_ll_convenience + libfuseprivate) -
# exactly what the upstream squashfuse_ll binary links - because our
# src/uenv/rootless.cpp plays the role of ll_main.c. Re-check it against the
# tarball's Makefile.am (*_la_SOURCES) when bumping the version.

project(
    'squashfuse',
    'c',
    version: '0.6.2',
    license: 'BSD-2-Clause',
    meson_version: '>=1.1',
    # squashfuse's own sources do not build clean under the top-level project's
    # -Wpedantic -Werror; treat it as a vendored library, like the other wraps.
    default_options: ['warning_level=0', 'werror=false'],
)

if host_machine.system() != 'linux'
    error('this squashfuse wrap only supports linux hosts')
endif

cc = meson.get_compiler('c')

# zstd and zlib from their wrapdb subprojects (configured before us at the top
# level, so the [provide] overrides are in place); fuse3 from the system - ll.c /
# fuseprivate.c include <fuse_lowlevel.h>.
libzstd_dep = dependency('libzstd')
zlib_dep = dependency('zlib')
fuse3_dep = dependency('fuse3', required: true)
threads_dep = dependency('threads')

#
# config.h
#
# Fixed values for a modern glibc/linux host, except the FUSE-version-sensitive
# declaration checks, which are probed against the fuse3 headers so a different
# libfuse3 (CI vs Alps) still gets correct values.
#
conf = configuration_data()
conf.set('FUSE_USE_VERSION', 32)

fuse_probe_args = ['-DFUSE_USE_VERSION=32']
foreach d : [
    ['HAVE_DECL_FUSE_ADD_DIRENT', 'fuse_add_dirent'],
    ['HAVE_DECL_FUSE_ADD_DIRENTRY', 'fuse_add_direntry'],
    ['HAVE_DECL_FUSE_CMDLINE_HELP', 'fuse_cmdline_help'],
    ['HAVE_DECL_FUSE_DAEMONIZE', 'fuse_daemonize'],
    ['HAVE_DECL_FUSE_SESSION_REMOVE_CHAN', 'fuse_session_remove_chan'],
]
    has = cc.has_header_symbol('fuse_lowlevel.h', d[1],
                               args: fuse_probe_args, dependencies: fuse3_dep)
    conf.set(d[0], has ? 1 : 0)
endforeach
# Not a decl check - true for every fuse3. Stated rather than probed.
conf.set('HAVE_FUSE_LL_FORGET_OP_64T', 1)

# platform headers, probed so the config is right off the build host
foreach h : [
    ['HAVE_ASM_BYTEORDER_H', 'asm/byteorder.h'],
    ['HAVE_DLFCN_H', 'dlfcn.h'],
    ['HAVE_ENDIAN_H', 'endian.h'],
    ['HAVE_INTTYPES_H', 'inttypes.h'],
    ['HAVE_STDINT_H', 'stdint.h'],
    ['HAVE_STDIO_H', 'stdio.h'],
    ['HAVE_STDLIB_H', 'stdlib.h'],
    ['HAVE_STRINGS_H', 'strings.h'],
    ['HAVE_STRING_H', 'string.h'],
    ['HAVE_SYS_STAT_H', 'sys/stat.h'],
    ['HAVE_SYS_SYSMACROS_H', 'sys/sysmacros.h'],
    ['HAVE_SYS_TYPES_H', 'sys/types.h'],
    ['HAVE_UNISTD_H', 'unistd.h'],
    ['HAVE_WCHAR_H', 'wchar.h'],
]
    if cc.has_header(h[1])
        conf.set(h[0], 1)
    endif
endforeach
if cc.has_header_symbol('linux/types.h', '__le16')
    conf.set('HAVE_LINUX_TYPES_LE16', 1)
endif

conf.set('HAVE_LIBPTHREAD', 1)
conf.set('STDC_HEADERS', 1)
conf.set('SQFS_MULTITHREADED', 1)
# libzstd_dep / zlib_dep put zstd.h / zlib.h on the include path for
# decompress.c, enabling CAN_DECOMPRESS_ZSTD and CAN_DECOMPRESS_ZLIB.
conf.set('HAVE_ZSTD_H', 1)
conf.set('HAVE_ZLIB_H', 1)

conf.set_quoted('LT_OBJDIR', '.libs/')
conf.set_quoted('PACKAGE', 'squashfuse')
conf.set_quoted('PACKAGE_BUGREPORT', 'dave@vasilevsky.ca')
conf.set_quoted('PACKAGE_NAME', 'squashfuse')
conf.set_quoted('PACKAGE_STRING', 'squashfuse ' + meson.project_version())
conf.set_quoted('PACKAGE_TARNAME', 'squashfuse')
conf.set_quoted('PACKAGE_URL', '')
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('VERSION', meson.project_version())

configure_file(input: 'config.h.in', output: 'config.h', configuration: conf)

#
# the low-level library
#
# Sources are the union of the three convenience archives the upstream
# squashfuse_ll binary links. implicit_include_directories (on by default) puts
# both the source dir (headers, .c) and the build dir (generated config.h) on
# the include path, so the sources' quote-includes resolve without more wiring.
#
sources = files(
    # libsquashfuse_convenience
    'swap.c', 'cache.c', 'cache_mt.c', 'table.c', 'dir.c', 'file.c', 'fs.c',
    'decompress.c', 'xattr.c', 'hash.c', 'stack.c', 'traverse.c', 'util.c',
    'nonstd-pread.c', 'nonstd-stat.c',
    # libsquashfuse_ll_convenience
    'll.c', 'll_inode.c', 'nonstd-daemon.c',
    # libfuseprivate (ll.c uses sqfs_listxattr / sqfs_stat from here)
    'fuseprivate.c', 'nonstd-makedev.c', 'nonstd-enoattr.c', 'stat.c',
)

libsquashfuse_ll = static_library(
    'squashfuse_ll',
    sources,
    c_args: ['-D_GNU_SOURCE', '-D_FILE_OFFSET_BITS=64'],
    dependencies: [libzstd_dep, zlib_dep, fuse3_dep, threads_dep],
    pic: true,
    install: false,
)

#
# public headers for the consumer
#
# src/uenv/rootless.cpp includes <squashfuse/ll.h>. squashfuse's headers are
# flat and generically named (file.h, dir.h, hash.h, ...), so putting the
# source dir on a consumer's include path would shadow unrelated headers across
# the whole uenv build. Stage the public headers (plus the generated config.h)
# under a squashfuse/ subdirectory at configure time and expose only its parent
# via -I, so <squashfuse/ll.h> resolves and the generic names stay namespaced.
#
inc_root = meson.current_build_dir() / 'include'
sqfs_incdir = inc_root / 'squashfuse'
run_command('mkdir', '-p', sqfs_incdir, check: true)
foreach h : [
    'll.h', 'squashfuse.h', 'common.h', 'dir.h', 'file.h', 'fs.h',
    'traverse.h', 'util.h', 'xattr.h', 'cache.h', 'table.h', 'stack.h',
    'hash.h', 'decompress.h', 'swap.h', 'squashfs_fs.h', 'nonstd.h',
    'nonstd-internal.h', 'fuseprivate.h', 'stat.h',
]
    run_command('cp', meson.current_source_dir() / h, sqfs_incdir / h, check: true)
endforeach
run_command('cp', meson.current_build_dir() / 'config.h', sqfs_incdir / 'config.h', check: true)

squashfuse_ll_dep = declare_dependency(
    link_with: libsquashfuse_ll,
    compile_args: ['-I' + inc_root],
    dependencies: [libzstd_dep, zlib_dep, fuse3_dep, threads_dep],
)

summary(
    {
        'version': meson.project_version(),
        'compression': 'zstd, zlib',
        'link': 'static',
    },
    section: 'squashfuse',
)
