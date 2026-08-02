// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// D3D11-level write probe (Feature::D3D11WriteProbe / DUSK_D3D11_WRITE_PROBE).
//
// The Ayesha atlas cache (src/engines/phyre/atlas_fix.cpp) serves repeated font-atlas
// reads from a CPU snapshot, which is only safe if every write to those
// textures goes through the middleware lock the mod already hooks.
// AtlasCensus enumerates writers on that path. This probe enumerates the only
// other path a write could take: the game issuing it through the D3D11 API
// directly, bypassing the middleware -- and this mod's hooks on it -- entirely.
// A cache serving a snapshot the mod never invalidated for a write it never
// saw would fail silently, so this settles the question by enumeration at the
// device-context level rather than by inference from the middleware side.
//
// Engine-agnostic in the sense that the hooks assume nothing about
// PhyreEngine; it is Ayesha-only only because the capability matrix
// (game.cpp) says so. Installing it on the other two titles would just find
// nothing, since they have no font-atlas cache to protect.
namespace atfix {

// Installs the vtable hooks on the immediate context if
// Feature::D3D11WriteProbe is enabled; a no-op otherwise (and safe to call
// unconditionally). Idempotent -- only the first context passed in ever
// installs anything, the same one-shot idiom as main.cpp's hookPresent -- so
// it is safe to call after every successful D3D11CreateDevice /
// D3D11CreateDeviceAndSwapChain that returns a context.
void initializeD3D11WriteProbe(ID3D11DeviceContext* context);

// Called from the hooked Present. Logs a periodic summary every 300 frames; a
// no-op unless the probe installed. A zero-writes result is logged explicitly
// rather than left as silence, which is indistinguishable from "the probe
// never ran".
void d3d11WriteProbeFrameTick();

}  // namespace atfix
