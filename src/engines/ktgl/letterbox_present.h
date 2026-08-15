// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

// KTGL's half of the letterbox: one pass at Present that fits the finished
// frame into a centred 16:9 rectangle. What the defect is and which games have
// it is in core/letterbox.h; this is only how the correction is applied on this
// engine, and why it cannot be applied the way Ayesha's would be.
//
// WHY A PASS RATHER THAN A NARROWED VIEWPORT. Confining the draw that fills the
// back buffer is cheaper and adds no resampling. It was built and it does not
// work here: this engine writes the back buffer FOUR TIMES A FRAME -- the
// composite, then SMAA's three passes, which run at Present over the finished
// frame and reach the surface through the mod's own hooks. Narrowing all four
// made each re-place what the previous wrote, and the picture appeared three
// times down the screen. Counted, not deduced: a probe logged four calls inside
// one 16.7 ms frame. No draw knows it is the last, so Present is the only point
// where "the frame is finished" is a fact.
//
// WHY A PRESENT-TIME PASS IS SAFE, given that two of supersampling's failed
// predecessors blacked the screen out with one. Their mistake was painting over
// a finished frame, not the timing: this reads a COPY and writes the original,
// which is what smaa.cpp already does at Present every frame on this engine.
//
// WHAT IT COSTS. One full-surface copy and one triangle, and a second resample:
// vertically a 4:3 session goes 1800 to 1200 to 900 rather than straight to 900.
// Correcting the composite instead would avoid that, but the composite only runs
// when supersampling is on.
namespace atfix {

// Registers the pass with core. Called from the KTGL install block; on any
// other engine nothing registers and core's letterbox declines.
void installKtglLetterbox();

}  // namespace atfix
