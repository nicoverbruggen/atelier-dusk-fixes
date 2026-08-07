// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

namespace atfix {

// Skip the movies played on the way to the title screen. Installs only when the
// capability matrix supports the feature and the user opted in. `exeBuild` is
// the verified build the Phyre module already resolved. Returns true when the
// hook is live.
bool installMovieSkip(BYTE* base, uint8_t exeBuild);

}  // namespace atfix
