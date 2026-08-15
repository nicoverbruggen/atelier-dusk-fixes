// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

// Character-separation diagnostics for Ayesha.
//
// THE DEFECT THIS EXISTS TO DEMONSTRATE. The routine that separates two
// overlapping character capsules computes its penetration depth and applies it
// without clamping at zero:
//
//     movss xmm3, [rdi+0xb4]      radius of body A
//     addss xmm3, [rbx+0xb4]      + radius of body B
//     divss xmm8, xmm9            1/d, to normalise the direction
//     subss xmm3, xmm9            depth = (rA+rB) - d      <-- never clamped
//
// A negative depth means the two bodies are FURTHER apart than their radii
// require, and applying it moves them toward each other. So this is an equality
// constraint rather than a separation constraint: whatever the starting
// distance, it solves to exactly rA+rB.
//
// WHY THE GAIN IS ON THE DEPTH AND NOT ON THE RADIUS. Scaling the combined
// radius was tried first and it is the wrong instrument, for a reason worth
// recording because it is not obvious: inflating the radius moves the
// equilibrium outward, so the player spends all their time INSIDE it, where the
// depth is positive and the routine behaves like any ordinary separation
// constraint. It repositions and then sits still. The defect lives in the
// negative branch, and widening the radius is precisely what stops that branch
// being reached.
//
// Scaling the depth reaches it. An equality constraint applied at unit gain is
// a projection: it lands on rA+rB in one step and stays there, which is why the
// vanilla defect is a shimmer rather than a spasm. Above a gain of two the same
// projection overshoots by more than the error it was correcting, so it crosses
// the equilibrium, computes a NEGATIVE depth on the far side, and is pulled
// back through -- oscillating instead of converging. That oscillation is the
// defect with its volume raised, and it cannot happen at all without the
// missing clamp, because a clamped constraint has nothing to apply once the
// bodies are apart.
//
// So the reading is: if a gain of 3 makes characters vibrate against each
// other, the negative branch is live and the diagnosis holds. If they simply
// separate harder and settle, it does not.
//
// HOW IT PATCHES. The `subss` is exactly five bytes, which is a jump and no
// filler, and nothing in the routine branches into it. The stub goes in the
// int3 alignment padding that follows -- 440 bytes on both builds, against the
// 22 this needs -- so no allocation is involved and everything stays inside the
// module:
//
//     [pad+0]   subss xmm3, xmm9        the displaced instruction
//     [pad+5]   mulss xmm3, [rip -> pad+18]
//     [pad+13]  jmp back to the site + 5
//     [pad+18]  the gain, as a float
//
// The constant sits after the code rather than before it so that the stub's
// address and its first instruction are the same thing. The other order costs a
// crash the first time the jump target and the entry point are allowed to
// disagree.
//
// THE FIX AND THE DIAGNOSTIC ARE THE SAME PATCH. `maxss xmm3, 0` leaves the
// depth alone when it is positive and replaces it with zero when it is not, so
// overlapping bodies still separate and separated ones are left where they are.
// That is one opcode byte away from the gain above -- 0x5f against 0x59 -- and
// one constant, which is why a single builder emits both. The clamp is simply
// on and carries no ini key, because a defect correction is not a setting the
// player has to make; DUSK_CHARACTER_PULL=0 turns it off for a diagnostic run.
// It is the same repair the Arland project already carries in its own
// `field_collision_fix.cpp`.
//
// Only one of them can be installed, because both claim the same five bytes.
// DUSK_COLLIDE_GAIN wins when it is set: a session asking for the diagnostic
// wants to watch the defect rather than have it repaired, and the log says so
// rather than leaving the fix looking as though it failed.
namespace atfix {

// Installs the clamp, or the diagnostic when DUSK_COLLIDE_GAIN asks for one.
// Both claim the same five bytes, so only one of them can be in place; the
// diagnostic wins and says so, because a session that sets the variable wants
// to see the defect rather than have it repaired. A gain of 1 is the honest
// no-op and a useful control: it proves the stub runs without changing what the
// game does.
//
// Returns true only when a patch is in place. A failed byte-window check,
// padding that is not int3, and a feature that is off or unsupported all
// decline and log.
//
// Idempotent.
bool installFieldCollision(BYTE* base, uint8_t exeBuild);

}  // namespace atfix
