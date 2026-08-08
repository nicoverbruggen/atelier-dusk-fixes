// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

namespace atfix {

// Installs the `DUSK_WINPARTS_TRACE` diagnostic: logs where each interface part
// is placed in the 1920x1080 canvas, and tallies how many of those positions are
// whole numbers. Suppresses nothing and changes nothing the game does. Returns
// true only when the switch is set and the hook went in.
//
// See win_parts_trace.cpp for the question it answers.
bool installWinPartsTrace(BYTE* base, const KtglGame& game);

}  // namespace atfix
