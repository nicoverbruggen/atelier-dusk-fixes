// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace atfix {

// Size the game's window correctly when it is created, rather than resizing it
// afterwards. Installs only when supersampling's present clamp is active, which
// is the only configuration where the engine's window size is wrong. Returns
// true when the hook is live. See window_size.cpp.
bool installKtglWindowSize();

}  // namespace atfix
