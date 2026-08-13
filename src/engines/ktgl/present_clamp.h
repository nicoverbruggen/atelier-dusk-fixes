// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

#include "../../core/supersample_policy.h"

// KTGL's route to supersampling: the present-size clamp.
//
// A SECOND ROUTE TO A FEATURE THAT ALREADY WORKED, for the engine that has its
// own. The Ayesha design substitutes the mod's own downscale at the composite's
// sample, because that engine has no notion of rendering larger than it
// presents. KTGL does have one, and a run proved half of it works
// with no code at all -- setting `Setting.ini` to 5120x2880 on a 2560x1440 panel
// produced every full-frame target at 5120x2880, confirmed by the target census,
// 21 targets across 12 shapes with the blur pyramid chaining down from it.
//
// What that run also proved is why the picture was still aliased: the SWAP CHAIN
// came out at 5120x2880 too, so nothing in the game ever resolved it and the
// compositor was left to do the reduction. The engine's own present-size
// override (`dev+0x300c`/`+0x3010`, applied by a `cmovg` in device init) stayed
// at zero, and no writer feeding it from the ini was ever found.
//
// THE LEVER. Device init reads the back buffer, compares its size against the
// render size, and creates an offscreen at the render size when they DISAGREE --
// which is exactly the path we want. So rather than hunting for whatever writes
// the override, make the sizes disagree directly: clamp the swap chain to the
// display and leave the engine's render size alone. The engine's scene targets
// come from its own GetScreenWidth/GetScreenHeight accessors, not from the swap
// chain, so they stay at N x.
//
// This touches DXGI only. No mapped address, no prologue, no per-game table.
namespace atfix {

// KTGL's answers to the questions in supersample_policy.h. Handed to the
// dispatcher in engine.cpp; nothing else calls this.
const SsaaPolicy& ktglSsaaPolicy();

// Whether the clamp is engaged this session. Also read by window_size.cpp, which
// sizes the game's window to the clamped client area at the call that sets it.
bool ktglPresentClampEnabled();

// The size the clamp presents at. False when the clamp is not engaged or no
// valid display size can be resolved.
bool ktglClampedDisplaySize(unsigned int* width, unsigned int* height);

}  // namespace atfix
