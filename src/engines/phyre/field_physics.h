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
// Quantified on Ayesha from a capture of the atelier's interior steps: 12 to 18
// px of vertical excursion while the character is horizontally at rest. That
// shape is a gravity-versus-threshold sawtooth rather than a bob, and it is
// what settles this as a defect correction rather than a preference.
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
// DUSK_FIELD_TRACE=<lines> is a diagnostic rather than a fix: it prints each
// controller's position and its change since the previous frame, for that many
// lines, then stops. The budget exists because the questions it answers need
// consecutive frames, and an unbounded per-frame line fills the log in about a
// second at 200 Hz. Frames where a character did not move print nothing.
//
// DUSK_FIELD_TALK_FREEZE=1 addresses the conversation shimmer: an NPC the
// player has run into vibrates in place while the dialogue is open.
//
// WHAT CAUSES IT. The engine's physics world update walks every collision
// object once a frame and pushes overlapping characters apart, writing both the
// controller position and the scene node. The game's controller update then
// re-asserts the position FROM that node. In free play this settles within a
// frame or two, because the player yields and the overlap goes away. In the
// talk state neither actor may move: the player is pinned and the NPC is held
// at the conversation anchor, so the game re-creates the overlap every frame
// and the physics undoes it every frame, at the display refresh rate.
//
// THE FIX cancels the physics half. The position is already read at both ends
// of the controller update, so saving it at one end and restoring it at the
// other undoes exactly what happened in between and nothing else.
//
// IT IS GATED ON THE ENGINE'S OWN STATE, not on the shape of the movement. An
// earlier version inferred "a conversation is happening" from movement that
// opposed the update's, and it was retuned five times without ever holding:
// the thresholds that caught the defect also caught ordinary running, and the
// ones that excluded running excluded most of the defect. Hooking
// `clsFMStateTalk::update` replaces the whole question with an answer.
//
// DUSK_FIELD_WATCH=<n> goes one step further and names the code doing the
// moving. It marks the page holding the position read-only and logs the
// faulting instruction for up to n writes, as a module RVA that can be looked
// up. It arms itself on whichever character shows the oscillation signature, so
// it needs no timing against a conversation, and it disarms when the budget is
// spent. Requires DUSK_FIELD_TRACE, which is what detects the signature.
//
// Expect it to be slow while armed: protection is per page, so every write to
// any neighbouring field in the same 4 KB faults too.
//
// Read the delta rather than the position. A character oscillating shows the
// same magnitude alternating sign; one drifting and being snapped back shows
// small steps one way and a single large step against them.
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
