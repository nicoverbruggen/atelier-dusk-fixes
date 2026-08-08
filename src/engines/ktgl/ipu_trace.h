// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

namespace atfix {

// Installs the `DUSK_IPU_TRACE` diagnostic: logs each distinct 2D image the game
// loads, with its file name and the RVA of the caller that asked for it.
// Suppresses nothing. Returns true only when the switch is set and the hook went
// in. See ipu_trace.cpp for what the caller column is for.
bool installIpuTrace(BYTE* base, const KtglGame& game);

}  // namespace atfix
