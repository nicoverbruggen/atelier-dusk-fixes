// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

namespace atfix {

// Skip the movie played on the way to the title screen. Installs only when the
// capability matrix supports the feature and the user opted in. Returns true
// when both hooks are live.
//
// Two hooks, and the pairing matters: see movie_skip.cpp.
bool installKtglMovieSkip(BYTE* base, const KtglGame& game);

}  // namespace atfix
