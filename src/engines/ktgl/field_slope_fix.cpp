// SPDX-License-Identifier: MIT
//
// See field_slope_fix.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "field_slope_fix.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/mem.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// bool __fastcall nspFM::clsFMICharacter::Update(chara*, float dt), vtable slot 3.
using CharaUpdateProc = bool (STDMETHODCALLTYPE*)(uintptr_t, float);
// void __fastcall <collision step>(body*, bool). The second argument is passed
// in dl and forwarded untouched.
using MoveCollideProc = void (STDMETHODCALLTYPE*)(uintptr_t, uint32_t);
// void __fastcall <apply delta>(body*, const float delta[4]). Adds the delta to
// the body position and publishes the result, which is why the fix uses it
// rather than writing the scene node itself.
using ApplyDeltaProc = void (STDMETHODCALLTYPE*)(uintptr_t, const float*);

CharaUpdateProc originalCharaUpdate = nullptr;
MoveCollideProc originalMoveCollide = nullptr;
ApplyDeltaProc applyDelta = nullptr;

// Resolved at install, so the hooks never recompute an address.
uintptr_t brainUserVtable = 0;

// Offsets on nspFM::clsFMICharacter.
constexpr uintptr_t kCharaBrain = 0x130;      // nspFM::clsFMIBrain*
constexpr uintptr_t kCharaVelocity = 0x150;   // vec4; X at +0x150, Z at +0x158

// Offsets on nspFM::clsFMBrainUser.
//
// kBrainCommand is the movement the brain asks for this frame, as a delta
// rather than a position: the character update copies it into the collision
// body's pending-movement field, and the collision step adds that to the body
// position rather than assigning it. It is read before the collision step runs,
// so the slide -- which the collision step creates -- is not in it. That is what
// makes it usable as "did the player ask to move", where the velocity vector is
// not: the velocity reads zero whether the player is walking or standing.
//
// kBrainPhysicsFlag is the byte the brain's own predicate returns to select the
// engine's gravity path. Logged rather than gated on, because an instrumented
// run found it clear on every sampled frame of ordinary field play.
constexpr uintptr_t kBrainCommand = 0xfc;     // vec4; X at +0xfc, Z at +0x104
constexpr uintptr_t kBrainPhysicsFlag = 0x12c;

// Offsets on the collision body.
constexpr uintptr_t kBodyPosition = 0x28;     // vec4
constexpr uintptr_t kBodyUpAxis = 0x38;       // vec4, (0, 1, 0) from the ctor
constexpr uintptr_t kBodyContactCount = 0x88;
constexpr uintptr_t kBodyContacts = 0x98;     // array of contact records
constexpr size_t kContactStride = 0x1c;       // normal at +0x00, depth at +0x10

// A contact counts as the ground when its normal leans this far towards up.
// cos(80 degrees); anything flatter than that is a wall as far as this is
// concerned, and the engine's own support test is useless here because it ships
// with its limit at 90 degrees and accepts everything.
constexpr float kGroundDot = 0.173648f;

// Below this the contact is flat enough that there is no downhill to speak of,
// and nothing is cancelled. About half a degree of tilt.
constexpr float kSlopeEpsilon = 0.01f;

// Commanded horizontal speed at or below this counts as none. The brain writes
// the velocity vector outright rather than accumulating into it, so a standing
// character's horizontal lanes are the zero the brain wrote and this only has to
// absorb the representation, not a drift.
constexpr float kCommandEpsilon = 1.0e-4f;

// A put-back smaller than this is not worth a publish.
constexpr float kHoldEpsilon = 1.0e-6f;

// Set for the duration of one character update, read by the collision hook.
// Thread-local rather than global because the flag is only meaningful inside the
// call that set it, and a second thread entering the collision step must not see
// another thread's decision.
thread_local bool t_holdThisCharacter = false;

bool fixEnabled() {
  return featureEnabled(Feature::FieldSlopeHold);
}

bool traceEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("DUSK_SLOPE_TRACE");
    return v && v[0] != '0';
  }();
  return on;
}

bool readVec3(uintptr_t addr, float& x, float& y, float& z) {
  return tryRead(addr, x) && tryRead(addr + 4, y) && tryRead(addr + 8, z);
}

// The direction a ball would roll, as a unit vector in the plane perpendicular
// to up. Derived rather than guessed: projecting gravity onto a plane of upward
// normal `n` leaves a horizontal part proportional to `n` minus its own up
// component, so the normal's own sideways lean already points downhill.
//
// Returns false when there is no ground contact, or when the one there is lies
// flat enough that no direction is meaningful. Both mean nothing is cancelled.
bool downhillDirection(uintptr_t body, float& dx, float& dy, float& dz) {
  float ux = 0.0f, uy = 0.0f, uz = 0.0f;
  if (!readVec3(body + kBodyUpAxis, ux, uy, uz))
    return false;

  uintptr_t count = 0;
  uintptr_t contacts = 0;
  if (!tryRead(body + kBodyContactCount, count) ||
      !tryRead(body + kBodyContacts, contacts) || !count || !contacts)
    return false;

  // The most ground-like contact, which is the one leaning furthest towards up.
  // A character standing on a slope while brushing a wall has both, and only the
  // ground one has a downhill.
  float bestDot = kGroundDot;
  float nx = 0.0f, ny = 0.0f, nz = 0.0f;
  bool found = false;
  for (uintptr_t i = 0; i < count && i < 64; ++i) {
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    if (!readVec3(contacts + i * kContactStride, cx, cy, cz))
      break;
    const float dot = cx * ux + cy * uy + cz * uz;
    if (dot > bestDot) {
      bestDot = dot;
      nx = cx; ny = cy; nz = cz;
      found = true;
    }
  }
  if (!found)
    return false;

  // Strip the up component, leaving the sideways lean.
  dx = nx - bestDot * ux;
  dy = ny - bestDot * uy;
  dz = nz - bestDot * uz;
  const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (length <= kSlopeEpsilon)
    return false;   // flat ground: no downhill, nothing to cancel

  dx /= length;
  dy /= length;
  dz /= length;
  return true;
}

// The conditions from the header, in the order that rejects soonest. When
// tracing, every player frame is reported with the values behind the verdict, so
// a run says why the gate did or did not fire rather than only that it did.
bool shouldHold(uintptr_t chara) {
  if (!fixEnabled() || !chara || !brainUserVtable)
    return false;

  uintptr_t brain = 0;
  if (!tryRead(chara + kCharaBrain, brain) || !brain)
    return false;

  // The brain's own vtable, against the address RTTI gives for this build. Only
  // the player has a clsFMBrainUser, so every NPC and enemy leaves here.
  uintptr_t brainVtable = 0;
  if (!tryRead(brain, brainVtable) || brainVtable != brainUserVtable)
    return false;

  // What the player asked for this frame. Zero horizontally means the slide the
  // collision step is about to produce is the engine's and not the player's.
  float cx = 0.0f, cy = 0.0f, cz = 0.0f;
  if (!readVec3(brain + kBrainCommand, cx, cy, cz))
    return false;

  const bool hold =
    std::fabs(cx) <= kCommandEpsilon && std::fabs(cz) <= kCommandEpsilon;

  if (traceEnabled()) {
    static unsigned seen = 0;
    if ((seen++ % 60) == 0) {
      uint8_t physics = 0;
      tryRead(brain + kBrainPhysicsFlag, physics);
      float vx = 0.0f, vy = 0.0f, vz = 0.0f;
      readVec3(chara + kCharaVelocity, vx, vy, vz);
      log("SLOPE_GATE hold=", hold ? 1 : 0, " cmd_x=", cx, " cmd_z=", cz,
          " physics=", int(physics), " vx=", vx, " vz=", vz);
    }
  }
  return hold;
}

bool STDMETHODCALLTYPE tracedCharaUpdate(uintptr_t chara, float dt) {
  // Saved and restored rather than cleared, so a nested update -- which the
  // player's own override performs, calling this body directly -- cannot leave
  // the flag set for whatever runs next.
  const bool previous = t_holdThisCharacter;
  t_holdThisCharacter = shouldHold(chara);
  const bool result = originalCharaUpdate(chara, dt);
  t_holdThisCharacter = previous;
  return result;
}

void STDMETHODCALLTYPE tracedMoveCollide(uintptr_t body, uint32_t flag) {
  float bx = 0.0f, by = 0.0f, bz = 0.0f;
  if (!t_holdThisCharacter || !body || !applyDelta ||
      !readVec3(body + kBodyPosition, bx, by, bz)) {
    originalMoveCollide(body, flag);
    return;
  }

  // The engine's step runs completely untouched, including its own publish.
  originalMoveCollide(body, flag);

  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  if (!readVec3(body + kBodyPosition, ax, ay, az))
    return;

  // What the step moved the character by, this frame.
  const float mx = ax - bx;
  const float my = ay - by;
  const float mz = az - bz;

  float hx = 0.0f, hy = 0.0f, hz = 0.0f;
  if (!downhillDirection(body, hx, hy, hz))
    return;   // flat, or no ground contact: leave the frame alone

  // Only the part of the movement that runs downhill. Anything across or up the
  // slope is someone else's doing -- another character leaning on this one, a
  // wall pushing it clear -- and characters do block each other in these games,
  // so cancelling the whole horizontal step would eat those too.
  const float along = mx * hx + my * hy + mz * hz;
  if (along <= kHoldEpsilon)
    return;   // not moving downhill

  const float delta[4] = { -along * hx, -along * hy, -along * hz, 0.0f };
  applyDelta(body, delta);

  if (traceEnabled()) {
    static unsigned held = 0;
    if ((held++ % 60) == 0) {
      log("SLOPE_HOLD held=", held, " downhill=", along,
          " dir=(", hx, ",", hy, ",", hz, ")",
          " moved=(", mx, ",", my, ",", mz, ")");
    }
  }
}

}  // namespace

bool installFieldSlopeHold(BYTE* base, const SlopeHoldTarget& target) {
  if (!fixEnabled()) {
    log("FIXES slope_hold=off");
    return false;
  }
  if (!base || !target.charaUpdateRva || !target.moveCollideRva ||
      !target.applyDeltaRva || !target.brainUserVtableRva) {
    log("FIXES slope_hold=unavailable (no address row for this executable)");
    return false;
  }

  BYTE* charaUpdate = base + target.charaUpdateRva;
  BYTE* moveCollide = base + target.moveCollideRva;
  if (!matches(charaUpdate, target.charaUpdateExpected)) {
    log("FIXES slope_hold=declined (character update prologue mismatch at"
        " rva=0x", std::hex, target.charaUpdateRva, std::dec, ")");
    return false;
  }
  if (!matches(moveCollide, target.moveCollideExpected)) {
    log("FIXES slope_hold=declined (collision step prologue mismatch at rva=0x",
        std::hex, target.moveCollideRva, std::dec, ")");
    return false;
  }

  brainUserVtable = reinterpret_cast<uintptr_t>(base) + target.brainUserVtableRva;
  applyDelta =
    reinterpret_cast<ApplyDeltaProc>(base + target.applyDeltaRva);

  // Both hooks or neither. The character hook alone would set a flag nothing
  // reads, and the collision hook alone would read a flag nothing sets, so a
  // half-installed pair is inert in one direction and wrong in the other.
  HookTransaction transaction;
  bool created = transaction.create(charaUpdate,
    reinterpret_cast<void*>(&tracedCharaUpdate),
    reinterpret_cast<void**>(&originalCharaUpdate));
  created = created && transaction.create(moveCollide,
    reinterpret_cast<void*>(&tracedMoveCollide),
    reinterpret_cast<void**>(&originalMoveCollide));

  const bool enabled = created && transaction.enableAll();
  if (!enabled) {
    const HookTransactionFailure failure = transaction.failure();
    log("SLOPE_HOLD transaction failed stage=",
        hookTransactionStageName(failure.stage), " status=", failure.status);
    if (!transaction.rollback()) {
      const HookTransactionFailure rollback = transaction.rollbackFailure();
      log("SLOPE_HOLD rollback_incomplete stage=",
          hookTransactionStageName(rollback.stage), " status=",
          rollback.status);
    }
    brainUserVtable = 0;
    applyDelta = nullptr;
  } else {
    transaction.commit();
  }

  log("FIXES slope_hold=", enabled ? "active" : "failed",
      " chara_rva=0x", std::hex, target.charaUpdateRva,
      " move_rva=0x", target.moveCollideRva,
      " brain_vtable=0x", target.brainUserVtableRva, std::dec,
      traceEnabled() ? " trace=on" : "");
  return enabled;
}

}  // namespace atfix
