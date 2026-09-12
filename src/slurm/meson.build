conf_data = configuration_data()
configure_file(input : 'config.hpp.in',
               output : 'config.hpp',
               configuration : conf_data)

subdir('slurm')
module_src = [
  'elastic.cpp',
  'environ.cpp',
  'mount_slurm.cpp',
  'plugin.cpp',
]

cpp_args_list = ['-Wall', '-Wpedantic', '-Wextra']
if use_fuse
    module_src += ['plugin_fuse.cpp']
else
    module_src += ['plugin_kernel.cpp']
endif


shared_module('slurm-uenv-mount',
              sources: module_src,
              dependencies: [uenv_dep, slurm_dep],
              cpp_args: cpp_args_list,
              install: true)
