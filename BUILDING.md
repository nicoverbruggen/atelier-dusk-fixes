# Building

The mod builds with Meson and Ninja on Windows (MSVC) or Linux (MinGW cross build). There are three targets across two architectures, because the split is in the games themselves: the six game executables are 64-bit and all six front-ends are 32-bit.

- `build64/d3d11.dll` — the game DLL, loaded by the games.
- `build64/dusk-fix-launcher.exe` — the mod's launcher window.
- `build32/msimg32.dll` — the launcher proxy, loaded by the games' own launcher and settings-editor programs.

The root `VERSION` file is the single version source for the Meson project and the Windows version resource. The build generates the numeric and string resource forms automatically.

`scripts/build_linux.sh` cross-compiles all three targets with MinGW inside the build container shared with the Arland project (set `$ATFIX_CONTAINER` to use a different container name). It takes an optional build type (default `release`), verifies that all three artifacts landed, and finishes by running `scripts/check_exports.py` over both DLLs. That check is not a formality: both are proxies, so a missing or stdcall-decorated export is not a build error but a game that fails to start with no explanation.

## Windows

Install Visual Studio 2022 with the Desktop development with C++ workload, Python, Meson, and Ninja. From an x64 Native Tools Developer PowerShell:

```powershell
meson setup build64 --buildtype release
meson compile -C build64
```

And from an x86 Native Tools Developer PowerShell, for the launcher proxy:

```powershell
meson setup build32 --buildtype release
meson compile -C build32
```

## Linux

On Fedora or another Linux distribution with MinGW (both the 64-bit and the 32-bit toolchain), Meson, and Ninja:

```sh
meson setup build64 --cross-file build-win64.txt --buildtype release
ninja -C build64
meson setup build32 --cross-file build-win32.txt --buildtype release
ninja -C build32
```

## Continuous integration

GitHub Actions builds the x64 targets and the x86 launcher proxy with MSVC, checks the required proxy exports, assembles the release archive, and publishes it for version tags. The workflow is `.github/workflows/build.yml`. The local Linux cross-build remains the day-to-day development path and produces the same three artifacts.

## Deploying

Copy `build64/d3d11.dll` next to the game executable. Add `build64/dusk-fix-launcher.exe` and `build32/msimg32.dll` beside it for the launcher; both are optional, and with either missing the stock launcher comes up as before. Nothing is written to the games' own executables; all patching is in memory and gated on fingerprints.

The launcher proxy is silent by design. To have it write `dusk-launcher.log` beside the game, add `-DDUSK_LAUNCHER_DIAGNOSTIC` to the 32-bit target's `cpp_args`.

## Release checklist

Before the first public release, build all three targets with `scripts/build_linux.sh`, run `scripts/check_exports.py` on the resulting DLLs, and complete the launcher validation session listed in `TECHNICAL.md`. The Ayesha fixes also require a clean cache-verification run and a 4K target-census run for the release record. Escha & Logy and Shallie should be tested to confirm that their controls remain disabled and their engine-specific fixes remain hard-off.
