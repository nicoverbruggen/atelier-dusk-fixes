project('atelier-dusk-fixes', ['c', 'cpp'],
  version : run_command(
    'python3', 'scripts/read_version.py', 'VERSION',
    check : true).stdout().strip(),
  default_options : ['cpp_std=c++17', 'buildtype=release', 'strip=true'])

cpp = meson.get_compiler('cpp')
windows = import('windows')   # compiles the DLL VERSIONINFO resources (windres / rc)
common_cpp_args = ['-DNOMINMAX', '-D_WIN32_WINNT=0x0a00']
common_link_args = cpp.get_id() == 'gcc' ? ['-static', '-static-libgcc', '-static-libstdc++', '-Wl,--strip-all'] : []

version_parts = meson.project_version().split('.')
if version_parts.length() != 3
  error('VERSION must contain three numeric components: X.Y.Z')
endif
version_data = configuration_data()
version_data.set('VERSION_COMMA', ','.join(version_parts) + ',0')
version_data.set_quoted('VERSION_STRING', meson.project_version() + '.0')
version_data.set_quoted('VERSION_SHORT', meson.project_version())
version_rc = configure_file(
  output : 'version.rc',
  configuration : version_data)
version_header = configure_file(
  input : 'src/version.h.in',
  output : 'version.h',
  configuration : version_data)
resource_includes = include_directories('.')

# DXGI interface GUIDs. MSVC needs dxguid.lib explicitly; MinGW happens to
# resolve them elsewhere.
dxguid_dep = cpp.find_library('dxguid')

sources = files(
  'src/main.cpp',
  'src/game.cpp',
  'src/atlas_stats.cpp',
  'src/hook_util.cpp',
  'vendor/minhook/src/hde/hde64.c',
  'vendor/minhook/src/hook.c',
  'vendor/minhook/src/buffer.c',
  'vendor/minhook/src/trampoline.c',
)

shared_library('d3d11',
  [sources,
   windows.compile_resources(
     'src/d3d11.rc',
     depend_files : version_rc,
     include_directories : resource_includes)],
  name_prefix : '',
  vs_module_defs : 'src/d3d11.def',
  include_directories : include_directories('src'),
  dependencies : dxguid_dep,
  cpp_args : common_cpp_args,
  link_args : common_link_args + (cpp.get_id() == 'gcc' ? ['-Wl,--file-alignment=4096'] : []),
  install : true)
