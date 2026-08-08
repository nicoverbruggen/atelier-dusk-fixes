// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

namespace atfix {

// Skip the Gust and Koei Tecmo logo images shown on the way to the title
// screen. Installs only when the capability matrix supports the feature and the
// user opted in. Returns true when the hook is live and both logo rows were
// found in the running executable's own image table.
//
// NOT a port of the Ayesha logo skip. That one drives a logo object's phase
// field to its terminal value; these two games have no such object. See
// logo_skip.cpp for what is hooked instead and why.
bool installKtglLogoSkip(BYTE* base, const KtglGame& game);

}  // namespace atfix
