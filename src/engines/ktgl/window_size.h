// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Right-sizing the game window for Escha & Logy and Shallie before D3D starts.
//
// WHY THE WINDOW IS WRONG AT ALL. Supersampling on these two works by writing
// the game's own Setting.ini at base x factor, so the engine renders everything
// larger, and then clamping the swap chain back down to the base. The engine
// also sizes its WINDOW from that ini, so at 150% on a 1920x1080 base it asks
// for a 2880x1620 window to hold a 1920x1080 image. Fullscreen hides this
// because the display mode decides the size instead.
//
// WHY BEFORE D3D RATHER THAN AFTER. Correcting it once the swap chain exists
// works, but the player first sees the oversized window and then watches it
// change. The engine creates a placeholder window and assigns its real
// size through SetWindowPos before touching D3D11, so that call is the primary
// correction. CreateWindowExA covers a variant that supplies a size directly.
//
// WHAT IS SUBSTITUTED. The size handed to CreateWindowEx is the WINDOW rect,
// frame included, while the size that has to come out right is the CLIENT area.
// The frame is measured with AdjustWindowRectEx from the styles of the call
// being made, not assumed: a caption and border are not the same thickness on
// every theme or under every compositor, and guessing here would trade one
// wrong size for another.
//
// HOW THE WINDOW IS IDENTIFIED. The hooks change nothing unless the KTGL clamp
// route is active, which requires one of these games with supersampling on, and
// the requested client area is larger than the configured display size. The
// CreateWindowExA route additionally requires a top-level window created by the
// game module. Smaller utility windows pass through unchanged.
//
// The post-creation fit in core (`ssaaFitOutputWindow`, core/supersample.h)
// remains, and covers a window this hook did not see. It returns immediately
// when the client area is already right.
namespace atfix {

// Size the game's window correctly when it is created, rather than resizing it
// afterwards. Installs only when supersampling's present clamp is active, which
// is the only configuration where the engine's window size is wrong. Returns
// true when the hook is live.
bool installKtglWindowSize();

}  // namespace atfix
