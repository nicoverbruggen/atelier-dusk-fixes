// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "phyre.h"

// Frame-rate-independent travel-map cursor movement for Ayesha.
//
// THE DEFECT is the one the Arland project already fixed in Totori and Meruru,
// reported here in game and then confirmed in the binary. Ayesha's mover reads
// the stick axes, folds in the four digital directions, rotates that direction
// by the map heading, and adds the result straight to the cursor position --
// with no delta time anywhere in the addition. That is a fixed distance per
// rendered FRAME, so the cursor crosses the map roughly three times faster at
// 200 Hz than the game was built for. The immediate caller does receive the
// real frame dt; the mover simply never consumes it.
//
// One difference from Arland worth knowing before reading the source: Ayesha's
// mover does NOT normalize the direction, so the step is |stick| * speed rather
// than a unit vector. It scales with stick deflection, which changes nothing
// about the fix -- whatever step the mover produced is rescaled -- but it does
// mean the shape the Arland search looked for (a packed rsqrtps normalize) is
// not in this binary, which is why the first two scans for it found nothing.
//
// THE FIX, also the Arland project's: capture the caller's dt, then rescale the
// step the mover produced by min(dt * 60, 1). At 60 fps and below that factor
// is 1 and the shipped behaviour is preserved exactly; above it, the same
// distance is covered per second rather than per frame. Nothing is predicted or
// simulated -- the mover runs untouched and its output is scaled afterwards,
// which is why this cannot desynchronise from whatever else reads that
// position.
//
// This is the same family as the field-jitter fix in field_physics.cpp: another
// movement value applied per frame instead of per second. Different subsystem,
// identical cause.
//
// NOT APPLICABLE EVERYWHERE. Rorona deliberately receives nothing in the Arland
// project: its travel map steps between discrete locations rather than moving a
// cursor continuously, and its selection cadence was measured unchanged between
// 144 and 60 fps. If Ayesha's map turns out to work that way too, this
// subsystem should install nothing rather than scale a step that is already
// correct.
namespace dusk {

// Installs the cursor fix. Returns true only when both hooks went in. Declines,
// with a logged reason, when the executable has no address row, when a prologue
// does not match, or when the feature is off.
bool installWorldMapFix(BYTE* base, uint8_t exeBuild);

}  // namespace dusk
