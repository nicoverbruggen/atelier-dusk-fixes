#!/usr/bin/env bash
# Day-to-day local build. Cross-compiles all three Windows targets with MinGW
# inside the build container, producing:
#   build64/d3d11.dll              (64-bit game DLL)
#   build64/dusk-fix-launcher.exe  (64-bit launcher window)
#   build32/msimg32.dll            (32-bit launcher proxy)
#
# The container (default "atfix-build", override with $ATFIX_CONTAINER) provides
# the MinGW-w64 toolchain, meson and ninja, and is shared with the Arland
# project. It is Fedora with meson, ninja and mingw64-gcc-c++.
# Optional first argument: the meson build type (default: release).
#
# The two targets exist because the split is in the games: all six Dusk
# front-ends are 32-bit and the six game executables are 64-bit.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
container="${ATFIX_CONTAINER:-atfix-build}"
buildtype="${1:-release}"

if ! podman container exists "$container"; then
    echo "Container '$container' does not exist: it is Fedora with meson, ninja and mingw64-gcc-c++." >&2
    exit 1
fi
if [[ "$(podman inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" != "true" ]]; then
    echo "Starting container '$container'"
    podman start "$container" >/dev/null
    # podman start returns before the container is ready to take exec sessions.
    for _ in $(seq 1 20); do
        podman exec "$container" true 2>/dev/null && break
        sleep 0.5
    done
fi

echo "Building in container '$container' ($buildtype) — $repo"
podman exec -i -w "$repo" "$container" bash -s "$buildtype" <<'EOSH'
set -e
buildtype="$1"
meson setup build64 --cross-file build-win64.txt --buildtype "$buildtype" --reconfigure >/dev/null 2>&1 \
  || meson setup build64 --cross-file build-win64.txt --buildtype "$buildtype"
meson compile -C build64
meson setup build32 --cross-file build-win32.txt --buildtype "$buildtype" --reconfigure >/dev/null 2>&1 \
  || meson setup build32 --cross-file build-win32.txt --buildtype "$buildtype"
meson compile -C build32
EOSH

echo
status=0
for artifact in build64/d3d11.dll build64/dusk-fix-launcher.exe build32/msimg32.dll; do
  if [[ -f "$repo/$artifact" ]]; then
    echo "  ok      $artifact"
  else
    echo "  MISSING $artifact" >&2
    status=1
  fi
done
[[ "$status" -eq 0 ]] || exit "$status"

# Both DLLs are proxies, so a missing or mis-decorated export is not a build
# error -- it is a game that fails to start with no explanation. MinGW is
# capable of exporting the 32-bit stdcall names decorated (_AlphaBlend@44),
# which the front-ends would not resolve.
python3 "$repo/scripts/check_exports.py" \
  "$repo/build64/d3d11.dll" "$repo/build32/msimg32.dll"
python3 "$repo/scripts/check_launcher_contract.py"
python3 "$repo/scripts/check_transaction_contract.py"
python3 "$repo/scripts/check_core_contract.py"
python3 "$repo/scripts/check_lifecycle_contract.py"
# Before packaging rather than after: the archive assembled below is what this
# check describes, so a licence it would ship without should stop the build
# instead of being found in the zip.
python3 "$repo/scripts/check_release_contract.py"

# Package the same archive the release workflow does, so what gets tested by
# hand locally has the same shape as what users download: same file names, same
# layout, documentation and licences in the same subfolder. Only the version in
# the name differs, since there is no tag to take it from.
#
# Kept out of the repo by /out/ in .gitignore.
if ! command -v zip >/dev/null 2>&1; then
  echo
  echo "zip is not installed; skipping the archive" >&2
  exit 0
fi

out="$repo/out"
stage="$out/stage"
rm -rf "$stage"
mkdir -p "$stage/dusk-fix/LICENSES"

# Shipped under its final name so the archive extracts straight into the game
# directory with nothing to rename.
cp "$repo/build64/d3d11.dll" "$repo/build64/dusk-fix-launcher.exe" \
   "$repo/build32/msimg32.dll" "$stage/"
cp "$repo/default.ini" "$stage/dusk-fix.ini"
cp "$repo/README.md" "$repo/LICENSE" "$stage/dusk-fix/"
# Named for what they cover: LICENSE points at them and there is no vendor/
# tree in the archive.
cp "$repo/vendor/minhook/LICENSE.txt" "$stage/dusk-fix/LICENSES/MinHook.txt"
cp "$repo/vendor/smaa/LICENSE.txt" "$stage/dusk-fix/LICENSES/SMAA.txt"

build_version="$(python3 "$repo/scripts/read_version.py" "$repo/VERSION")"
archive="$out/dusk-fix-$build_version.zip"
rm -f "$archive"
( cd "$stage" && zip -qr "$archive" \
    d3d11.dll dusk-fix-launcher.exe msimg32.dll dusk-fix.ini dusk-fix )
rm -rf "$stage"

echo
echo "  packaged out/$(basename "$archive")"
