sqlite3 = find_program('sqlite3', required: false)
if not sqlite3.found()
    error('sqlite3 must be installed to set up the test framework')
endif

integration_path = meson.current_source_dir() + '/integration'
setup_path = meson.current_source_dir() + '/setup'

bats_install_path = meson.current_build_dir() + '/external'
bats = custom_target(
    'bats-install',
    output: 'bats',
    command: [integration_path + '/install-bats', bats_install_path],
    build_by_default: true,
    install: false,
)

test_data_install_path = meson.current_build_dir() + '/data'
test_generated_data = custom_target (
    'test-generated-data',
    output: 'data',
    command: [setup_path + '/setup', test_data_install_path],
    build_by_default: true,
    install: false,
)

fs = import('fs')
fs.copyfile('integration/common.bash', 'common.bash')

# download zot for testing registries
zot_arch = (host_machine.cpu_family() == 'aarch64') ? 'arm64' : 'amd64'
zot_path = meson.current_build_dir() / 'zot'
custom_target('zot-binary',
    output: 'zot',
    command: [integration_path + '/install-zot', '@OUTPUT@', zot_arch],
    build_by_default: true,
    install: false,
)

if uenv_cli
  fs.copyfile('integration/cli.bats', 'cli.bats')
  # the registry suite drives uenv against a throwaway local zot registry.
  # registry_ctl (zot binary lifecycle) and listing_mock (uenv listing service
  # stand-in) are installed with execute permissions, like elastic_mock.
  custom_target('registry-ctl',
      input: 'integration/registry_ctl',
      output: 'registry_ctl',
      command: ['install', '-m', '755', '@INPUT@', '@OUTPUT@'],
      build_by_default: true,
      install: false,
  )
  custom_target('listing-mock',
      input: 'integration/listing_mock',
      output: 'listing_mock',
      command: ['install', '-m', '755', '@INPUT@', '@OUTPUT@'],
      build_by_default: true,
      install: false,
  )
  fs.copyfile('integration/registry.bats', 'registry.bats')
endif
if uenv_slurm_plugin
  custom_target('elastic-mock',
      input: 'integration/elastic_mock',
      output: 'elastic_mock',
      command: ['install', '-m', '755', '@INPUT@', '@OUTPUT@'],
      build_by_default: true,
      install: false,
  )
  fs.copyfile('integration/slurm.bats', 'slurm.bats')
endif
if uenv_squashfs_mount
  fs.copyfile('integration/squashfs-mount.bats', 'squashfs-mount.bats')
endif
# --join only exists in the fuse build, and the tests need Slurm to launch
# several tasks on one node (they self-skip when it is unavailable)
if use_fuse
  fs.copyfile('integration/join.bats', 'join.bats')
endif

suite_input = integration_path + '/setup_suite.bash.in'
suite_ouput = meson.current_build_dir() + '/setup_suite.bash'
setup_suite = custom_target (
    'setup-suite',
    input: suite_input,
    output: 'setup_suite.bash',
    command: [
        integration_path + '/install-setup_suite.sh',
        '@INPUT@',
        '@OUTPUT@',
        meson.global_source_root(),
        meson.global_build_root(),
    ],
    build_by_default: true,
    install: false,
)

unit_src = [
        'unit/archive.cpp',
        'unit/cgroup.cpp',
        'unit/curl.cpp',
        'unit/dates.cpp',
        'unit/env.cpp',
        'unit/envvars.cpp',
        'unit/fs.cpp',
        'unit/join_context.cpp',
        'unit/lex.cpp',
        'unit/main.cpp',
        'unit/mount.cpp',
        'unit/oci_auth.cpp',
        'unit/oci_client.cpp',
        'unit/oci_digest.cpp',
        'unit/oci_manifest.cpp',
        'unit/oci_parse.cpp',
        'unit/oci_registry.cpp',
        'unit/oci_tag.cpp',
        'unit/parse.cpp',
        'unit/proc_barrier.cpp',
        'unit/sha.cpp',
        'unit/shared_mapping.cpp',
        'unit/shell.cpp',
        'unit/signal.cpp',
        'unit/strings.cpp',
        'unit/ready_fork.cpp',
        'unit/repository.cpp',
        'unit/settings.cpp',
        'unit/setns.cpp',
        'unit/subprocess.cpp',
        'unit/tasks_per_node.cpp',
        'unit/telemetry.cpp',
        'unit/url.cpp',
        'unit/util_parse.cpp',
]

unit = executable('unit',
        sources: unit_src,
        dependencies: [catch_dep, uenv_dep],
        cpp_args: ['-DUENV_TEST_ZOT_PATH="' + zot_path + '"'],
        build_by_default: true,
        install: false)

test('unit', unit, is_parallel : false)
if uenv_cli
  cli_env = []
  if uenv_squashfs_mount
    cli_env += ['LSAN_OPTIONS=suppressions=' + integration_path + '/squashfs-mount-lsan.supp:print_suppressions=0']
  endif
  test('cli', bats, args: ['./test/cli.bats'], env: cli_env, is_parallel : false)
  test('registry', bats, args: ['./test/registry.bats'], is_parallel : false)
endif
if use_fuse
  test('join', bats, args: ['./test/join.bats'], is_parallel : false)
endif
if uenv_squashfs_mount
  test('squashfs-mount', bats, args: ['./test/squashfs-mount.bats'],
       env: ['LSAN_OPTIONS=suppressions=' + integration_path + '/squashfs-mount-lsan.supp:print_suppressions=0'],
       is_parallel : false)
endif
