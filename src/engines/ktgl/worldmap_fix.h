// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

#include "ktgl.h"

// Escha & Logy's travel-map cursor moves a fixed distance per frame.
//
// THE DEFECT, and it is one multiply. The cursor mover scales the stick vector
// by a constant and adds the result to the position once per frame, with no
// frame-time term anywhere between the stick read and the write. At 60 Hz that
// was the intended speed; at 200 Hz the cursor crosses the map 3.33 times too
// fast.
//
// SHALLIE IS THE PROOF, not an assumption. The same function in Shallie takes
// the frame delta as a parameter and multiplies by it, and the two builds are
// otherwise the same code. Read from the bytes of both executables:
//
//   Escha EN 0x399ec0   no float parameter used
//                       0xde4fd8 = 135.0, times [rax+0x3a0], onto the stick vector
//   Shallie EN 0x36bf50 movaps xmm6, xmm1        <- the dt parameter
//                       0xdd82b4 = 30.0, times xmm6, times 0x6df02c = 60.0
//
// So Shallie computes 30 * dt * 60 -- 1800 units per second at any refresh rate
// -- and Escha computes 135 * zoom per frame. `[rax+0x3a0]` is an eased scalar
// taking the values 1.0 and 0.5, written by WMMap::Update with its own target
// and remaining-time fields, which is what rules out its being a disguised
// delta time. That is the only thing the conclusion rests on.
//
// Shallie therefore gets nothing from this file, and the capability matrix
// hard-offs it there.
//
// THE CORRECTION is the Ayesha travel-map fix's, unchanged in mechanism: hook
// the driver to capture the dt it receives and never forwards, hook the mover,
// and rescale the step it applied by min(dt * 60, 1). At 60 fps and below the
// factor is 1 and the shipped behaviour is preserved bit for bit. See
// engines/phyre/worldmap_fix.h for the argument in full; only the address pack,
// the offsets and the publish differ.
//
// The potentially competing state was settled at runtime: 658 mover calls were
// observed while the player crossed the map, and the correction made the cursor
// speed stable at high refresh. The second, already-delta-correct mover belongs
// to a different movement mode and does not invalidate this route.
namespace atfix {

// Installs on Escha & Logy only, and declines with a reason on anything else or
// on a prologue mismatch. Safe to call when the feature is off: it returns
// false without hooking.
bool installKtglWorldMapFix(BYTE* base, const KtglGame& game);

}  // namespace atfix
