// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

namespace atfix {

// Skip the boot logos. Installs only when the capability matrix supports the
// feature and the user opted in. `exeBuild` is the verified build the Phyre
// module already resolved. Returns true when both hooks are live.
bool installLogoSkip(BYTE* base, uint8_t exeBuild);

}  // namespace atfix
