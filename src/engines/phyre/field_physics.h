// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

#include "../../core/hook_util.h"

namespace atfix {

// Ayesha field movement: two corrections, both on by default, sharing one detour
// on the controller's per-frame update. Ported from the Arland games, which run
// the same character controller under a different renderer -- every offset and
// address below was re-read from Ayesha's own binary rather than carried across.
//
// THE GROUND RAY fixes characters bouncing on uneven ground and glitching on
// stairs. The engine holds a character up by cancelling its vertical velocity on
// any frame it has ground contact, and keeps the grounded flag alive for a fixed
// 0.0666667 seconds after contact is lost -- without stopping gravity for that
// window. A character whose contact flickers free-falls while the engine still
// considers it grounded, and the whole accumulated drop is corrected in one
// frame when contact returns. The amplitude follows from a wall-clock constant,
// so it is the same at any frame rate; only its appearance changes.
//
// The correction casts a short ray down from the feet after the frame's
// movement, and where it finds ground, puts the character on it and takes its
// vertical velocity away. What is left is one gravity step per frame. Two
// details carry it: the ray runs AFTER the update, so the height matches where
// the character ends the frame rather than where it started, and the reach is
// short, so walking off a ledge misses it and falls normally.
//
// DUSK_FIELD_GROUND_RAY turns it off; DUSK_FIELD_GRACE_HOLD turns off the weaker
// fallback covering frames where the ray finds no ground. Verbose logging
// ([Diagnostics] VerboseLogging) samples why the ray did or did not correct,
// which is how a build gets re-checked.
//
// THE THRESHOLD RESCALE is a different defect that looks unrelated until it
// bites: the collision resolver discards any frame in which the character moves
// less than 0.0085 world units in total, reverting the position. That is a
// per-frame distance, so as a speed floor it rises with the frame rate, at
// 0.0085 * fps -- 0.51 units/s at 60, 1.70 at 200, and above running speed by
// 600, where the character stops moving entirely. Measured player speed in the
// Arland games is 1.9 to 2.3 units/s walking, so by 200 fps the floor has
// already reached ordinary walking pace. Rescaling the constant with the frame
// time pins the floor at its 60 fps value whatever the refresh.
//
// The ground ray does not help there and cannot: the revert restores the whole
// position vector, correction included.
//
// DUSK_FIELD_ENGINE_FIX turns it off.
//
// A resting stabilizer used to sit alongside the rescale, holding a character
// that was grounded, horizontally still, and whose last move the resolver threw
// away. The ground ray covers that case and more -- it reaches a character that
// is moving, which the stabilizer structurally could not -- so it was removed
// once the two were measured against each other.
//
// `exeBuild` selects the address pack (BuildEnglish / BuildMultilingual); the
// caller has already fingerprinted the executable.
bool installFieldPhysics(BYTE* base, uint8_t exeBuild);

}  // namespace atfix
