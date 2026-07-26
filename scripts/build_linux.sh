#!/usr/bin/env bash
# Day-to-day local build. Cross-compiles the Windows target with MinGW inside the
# build container, producing:
#   build64/d3d11.dll          (64-bit game DLL)
#
# The container (default "atfix-build", override with $ATFIX_CONTAINER) provides
# the MinGW-w64 toolchain, meson and ninja, and is shared with the Arland
# project -- see ../atelier-arland-fixes/BUILDING.md for how to create it.
# Optional first argument: the meson build type (default: release).
#
# Unlike the Arland project there is no 32-bit target: this repository ships no
# launcher proxy yet.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
container="${ATFIX_CONTAINER:-atfix-build}"
buildtype="${1:-release}"

if ! podman container exists "$container"; then
    echo "Container '$container' does not exist. See ../atelier-arland-fixes/BUILDING.md." >&2
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
EOSH

echo
if [[ -f "$repo/build64/d3d11.dll" ]]; then
  echo "  ok      build64/d3d11.dll"
else
  echo "  MISSING build64/d3d11.dll" >&2
  exit 1
fi
