// SPDX-License-Identifier: MIT
#pragma once

// Replaces the window class's grey background brush with black, so the startup
// flash before the first frame is black rather than mid-grey. See
// window_background.cpp for what the game does and why this is the whole fix.
//
// Must run before the game registers its window class, which means DllMain.
// Idempotent, and it installs nothing when the running executable is not one of
// the three games.
namespace atfix {

void installWindowBackgroundFix();

}  // namespace atfix
