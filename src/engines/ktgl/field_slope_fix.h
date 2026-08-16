// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>

// Holding a standing player still on a slope.
//
// THE DEFECT. A player character standing on a slope slides down it. Reported
// from play in Escha & Logy, seen at every frame rate tried on the reference
// machine, and seen on the Switch release, which runs at 30. So it is a defect
// in the shipped game rather than anything the frame rate introduces. What the
// frame rate changes is how fast the character slides, which is a second
// question this fix does not answer.
//
// WHY IT HAPPENS, as far as reading the code establishes. The engine's own
// character collision body carries two slope limits, both in degrees, and its
// constructor sets both to 90:
//
//   [body+0x70]  how far the collision response may turn the movement before
//                the resolver gives up and cancels the step
//   [body+0x74]  how far a contact normal may lie from the up axis and still
//                count as ground
//
// At 90 neither does anything. The support test becomes "is the normal not
// pointing downwards", so a wall counts as ground. The cancel needs the
// resolved direction to lie more than 90 degrees off the intended one, which
// for a character standing still is a reversal and does not happen: the only
// movement offered is gravity pointing straight down, and what comes back is a
// slide along the slope, which sits 90 minus the slope angle away from that. On
// a 30 degree slope that is 60 degrees, well inside the limit. Nothing else in
// the path applies friction. VERIFIED: the four constructor immediates are
// byte-identical in all four KTGL builds, and no writer of [body+0x70] was
// found anywhere in the image.
//
// THE CORRECTION. Rather than change either limit, which would alter how every
// slope in both games behaves, this holds the character's horizontal position
// still for exactly the frames where it should not be moving at all: the player
// is under direct control and has commanded no horizontal movement. Vertical
// motion is left alone, so the character still settles onto the ground, still
// falls, and still takes stairs and ledges normally.
//
// The gate is two conditions:
//
//   1. The character's brain is `nspFM::clsFMBrainUser`, compared against the
//      vtable address RTTI gives for that build. This is stated by the game
//      rather than inferred from a shape, and only the player has one. It also
//      matches a defect only ever reported for the player: the base
//      `clsFMIBrain` predicate that would put a character on the engine's
//      gravity path is `xor al, al; ret`, so no NPC or enemy can reach it.
//   2. `[brain+0xfc]`, the movement the brain asks for this frame, is zero
//      horizontally. It is a delta rather than a position -- the character
//      update copies it into the body's pending-movement field and the
//      collision step adds that to the body position -- and it is read before
//      the collision step runs, so the slide is not in it.
//
// TWO EARLIER GATES WERE WRONG, and both were settled by an instrumented run
// rather than by reading more code. Recorded because the reasoning behind each
// still looks sound on the page.
//
// The first used the character's velocity at `[chara+0x150]`, on the argument
// that gravity only touches Y so horizontal velocity must be commanded. That
// vector reads zero whether the player is walking or standing, because ordinary
// movement goes through the mover and never writes it. The hold therefore fired
// while walking and pinned the player in place: the log showed put-backs of 0.8
// to 2.5 units in a frame, which is walking pace.
//
// The second added `[brain+0x12c]`, the byte selecting the engine's gravity
// path, expecting walking to be excluded as the mover path. That byte was clear
// on all 56 sampled frames of ordinary field play, so the gate never fired at
// all and the slide was untouched. The sliding happens on the mover path, not
// under gravity.
//
// The same runs measured the defect. A standing player's put-back was 0.00549
// in X and 0.00665 in Z, frame after frame, the same numbers to six figures.
// A constant per rendered frame is a rate that scales with refresh, which is
// why the slide is faster at 200 than at 30 while still being present at 30 and
// on the Switch release.
//
// WHY IT HOOKS WHERE IT DOES. The horizontal movement is not in the velocity;
// it is created inside the collision step, by the resolver deflecting the
// downward step along the contact plane and by the depenetration push. That
// step publishes the new position to the scene node before it returns, so a
// correction applied afterwards has to republish. The engine has its own helper
// for exactly that -- add a delta to the body position and publish -- and the
// fix calls it rather than writing the node itself.
//
// So the shape is: remember the position, let the engine's step run untouched,
// and if it moved the character downhill when nothing asked it to, take that
// much back through the engine's own publish path. Nothing is predicted and no
// engine state is simulated, which is why this cannot disagree with whatever
// else reads the body.
//
// ONLY THE DOWNHILL COMPONENT IS TAKEN BACK, and this is not a refinement for
// its own sake. Characters block each other in these games, so another
// character leaning on a standing player reaches the position through the same
// depenetration the slide does. Cancelling the whole horizontal step would eat
// that too, along with anything else pressing on the body. Projecting onto the
// downhill direction leaves every component except the one gravity is
// responsible for, and on flat ground there is no downhill direction at all, so
// nothing is cancelled anywhere except on a slope.
//
// The direction is derived rather than guessed. Projecting gravity onto a plane
// of upward normal `n` leaves a horizontal part proportional to `n` minus its
// own up component, so the normal's sideways lean already points downhill. The
// body carries its contacts -- count at +0x88, array at +0x98, stride 0x1c,
// normal first -- and the ground one is whichever leans furthest towards the up
// axis at +0x38. The engine's own support test cannot be used for that, because
// it ships with its limit at 90 degrees and so accepts a wall.
//
// DUSK_SLOPE_TRACE=1 logs the commanded velocity and the horizontal distance
// put back, once every 60 held frames. It is a diagnostic and has no ini key.
namespace atfix {

// One executable's row. Supplied by the engine module, because address packs do
// not belong beside the mechanism.
struct SlopeHoldTarget {
  uintptr_t charaUpdateRva;    // nspFM::clsFMICharacter::Update, vtable slot 3
  std::array<BYTE, 16> charaUpdateExpected;
  uintptr_t moveCollideRva;    // the collision step the character update calls
  std::array<BYTE, 16> moveCollideExpected;
  uintptr_t applyDeltaRva;     // add a delta to the body position and publish
  uintptr_t brainUserVtableRva;  // nspFM::clsFMBrainUser, from that build's RTTI
};

// Installs the fix. Returns true only if both hooks went in. Declines, with a
// logged reason, when the feature is off, a row is empty, or either prologue
// does not match. A partial install is rolled back rather than left live.
bool installFieldSlopeHold(BYTE* base, const SlopeHoldTarget& target);

}  // namespace atfix
