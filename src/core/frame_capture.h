// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

// `DUSK_FRAME_CAPTURE=<n>`: write the back buffer to `dusk-frame-<n>.png` next
// to the log on frame n, and record a checksum of the pixels.
//
// A diagnostic, and the checksum is half its value. The other half is that the
// file can simply be looked at, which is the only honest way to judge a
// rendering change; but a change that alters nothing produces a byte-identical
// checksum, and that answer arrives without anyone having to look or reason. On
// 2026-08-10 an experiment cost a build and a boot before a tally left switched
// on by luck revealed it had changed nothing at all.
//
// A FIXED FRAME NUMBER RATHER THAN A DELAY, so two runs are comparable. Frame
// 600 holds the same content across runs; "five seconds in" drifts with load
// times. The startup logos and the opening movie are already skipped on all
// three games, so the title screen arrives well before any sensible n.
namespace atfix {

// Called from the hooked Present, before the frame is handed on. Does nothing
// unless the switch is set, and nothing after its one frame has been written.
void frameCaptureTick(IDXGISwapChain* swapChain);

// Dump an arbitrary texture to `<name>.png` beside the log. Used by the frame
// map, which is how the pre-UI moment was found: a surface copied in stream
// order after one draw showed the finished scene with no interface in it.
bool frameCaptureDumpTexture(ID3D11Texture2D* texture, const char* name);

}  // namespace atfix
