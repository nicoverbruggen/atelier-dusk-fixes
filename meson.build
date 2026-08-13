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
  input : 'src/core/version.h.in',
  output : 'version.h',
  configuration : version_data)
resource_includes = include_directories('.')

# A version number cannot distinguish a tagged release from a local build with
# additional changes. Regenerate the source stamp at build time so a diagnostic
# log identifies the exact commit and whether the working tree was dirty.
version_git_header = vcs_tag(
  input : 'src/core/version_git.h.in',
  output : 'version_git.h',
  # Local builds run inside a container whose uid differs from the checkout's.
  # This one-command override avoids Git's dubious-ownership refusal without
  # changing either the host or container configuration.
  command : ['git', '-c', 'safe.directory=*', 'describe', '--always',
             '--abbrev=8', '--dirty=-dirty'],
  fallback : 'unknown')

# Two targets, selected by the cross file's CPU family, exactly as the Arland
# project does it: the 32-bit build produces only the launcher proxy, because
# the Dusk front-ends are 32-bit while the games themselves are 64-bit.
if host_machine.cpu_family() == 'x86'
  shared_library('msimg32',
    ['src/launcher/launcher_proxy.cpp', version_git_header,
     windows.compile_resources(
       'src/launcher/msimg32.rc',
       depend_files : version_rc,
       include_directories : resource_includes)],
    name_prefix : '',
    vs_module_defs : 'src/launcher/msimg32.def',
    cpp_args : common_cpp_args,
    link_args : common_link_args,
    install : true)
else
  # DXGI interface GUIDs. MSVC needs dxguid.lib explicitly; MinGW happens to
  # resolve them elsewhere.
  dxguid_dep = cpp.find_library('dxguid')

  # Split by engine: src/core is engine-agnostic, src/engines/phyre is Ayesha
  # (PhyreEngine), src/engines/ktgl is Escha & Logy plus Shallie. One d3d11.dll
  # d3d11.dll -- see src/core/engine.h for why.
  sources = files(
    'src/core/main.cpp',
    'src/core/config.cpp',
    'src/core/crash_log.cpp',
    'src/core/module_lifetime.cpp',
    'src/core/engine.cpp',
    'src/core/game.cpp',
    'src/core/hook_util.cpp',
    'src/core/d3d11_hooks.cpp',
    'src/core/highres.cpp',
    'src/core/mix_card.cpp',
    'src/core/pad_rescan.cpp',
    'src/core/pad_notify_trace.cpp',
    'src/core/sampler.cpp',
    'src/core/scene_pass.cpp',
    'src/core/sharpen.cpp',
    'src/core/smaa.cpp',
    'src/core/supersample.cpp',
    'src/core/scene_policy.cpp',
    'src/core/window_background.cpp',
    'src/engines/phyre/phyre.cpp',
    'src/engines/phyre/atlas_fix.cpp',
    'src/engines/phyre/field_physics.cpp',
    'src/engines/phyre/logo_skip.cpp',
    'src/engines/phyre/movie_skip.cpp',
    'src/engines/phyre/scene_target.cpp',
    'src/engines/phyre/ssaa_policy.cpp',
    'src/engines/phyre/scene_policy.cpp',
    'src/engines/phyre/pre_ui.cpp',
    'src/engines/phyre/worldmap_fix.cpp',
    'src/engines/ktgl/ktgl.cpp',
    'src/engines/ktgl/loading_text_fix.cpp',
    'src/engines/ktgl/logo_skip.cpp',
    'src/engines/ktgl/movie_skip.cpp',
    'src/engines/ktgl/scene_target.cpp',
    'src/engines/ktgl/present_clamp.cpp',
    'src/engines/ktgl/scene_policy.cpp',
    'src/engines/ktgl/worldmap_fix.cpp',
    'src/engines/ktgl/system_save_fix.cpp',
    'src/engines/ktgl/window_size.cpp',
    'src/engines/ktgl/control_prompt_fix.cpp',
    'vendor/minhook/src/hde/hde64.c',
    'vendor/minhook/src/hook.c',
    'vendor/minhook/src/buffer.c',
    'vendor/minhook/src/trampoline.c',
  )

  shared_library('d3d11',
    [sources, version_git_header,
     windows.compile_resources(
       'src/core/d3d11.rc',
       depend_files : version_rc,
       include_directories : resource_includes)],
    name_prefix : '',
    vs_module_defs : 'src/core/d3d11.def',
    include_directories : include_directories('src', 'src/core'),
    dependencies : dxguid_dep,
    cpp_args : common_cpp_args,
    link_args : common_link_args + (cpp.get_id() == 'gcc' ? ['-Wl,--file-alignment=4096'] : []),
    install : true)

  # The mod's launcher window. 64-bit like the games it starts, which is also
  # what lets it CreateProcess them directly.
  #
  # win_subsystem selects the GUI subsystem (WinMain) for both MinGW
  # (-mwindows) and MSVC (/SUBSYSTEM:WINDOWS).
  executable('dusk-fix-launcher',
    ['src/launcher/launcher_gui.cpp', version_git_header,
     # Carries the ComCtl32 v6 manifest, which is what gets the themed common
     # controls rather than the flat 5.82 ones. See the manifest for the rest.
     windows.compile_resources(
       'src/launcher/dusk-fix-launcher.rc',
       depend_files : [
         version_rc,
         files('src/launcher/dusk-fix-launcher.manifest'),
       ],
       include_directories : resource_includes)],
    win_subsystem : 'windows',
    include_directories : include_directories('src'),
    dependencies : [
      cpp.find_library('comctl32'),
      # OpenThemeData/DrawThemeBackground, for sampling the visual style's tab
      # page colour so the controls on it match. See themedTabBodyColor.
      cpp.find_library('uxtheme'),
      cpp.find_library('shell32'),
      # GetFileVersionInfo, for reading the installed mod's version off
      # d3d11.dll rather than reporting this executable's own.
      cpp.find_library('version'),
    ],
    cpp_args : common_cpp_args,
    link_args : common_link_args,
    install : true)
endif
