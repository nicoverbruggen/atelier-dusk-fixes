# Building

The mod builds with Meson and Ninja on Windows (MSVC) or Linux (MinGW cross build). The output is `build64/d3d11.dll`. There is no 32-bit target: this repository ships no launcher proxy yet.

The root `VERSION` file is the single version source for the Meson project and the Windows version resource. The build generates the numeric and string resource forms automatically.

`scripts/build_linux.sh` cross-compiles with MinGW inside the build container shared with the Arland project (set `$ATFIX_CONTAINER` to use a different container name). It takes an optional build type (default `release`).

## Windows

Install Visual Studio 2022 with the Desktop development with C++ workload, Python, Meson, and Ninja. From an x64 Native Tools Developer PowerShell:

```powershell
meson setup build64 --buildtype release
meson compile -C build64
```

## Linux

On Fedora or another Linux distribution with MinGW, Meson, and Ninja:

```sh
meson setup build64 --cross-file build-win64.txt --buildtype release
ninja -C build64
```

## Deploying

Copy `build64/d3d11.dll` next to the game executable. Nothing is written to the game's own files; all patching is in memory and gated on executable fingerprints.
