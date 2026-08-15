// SPDX-License-Identifier: MIT
//
// Ayesha field movement: the addresses, and how each correction is applied.
//
// See field_physics.h for both defects, what each correction does about them,
// why the ray runs after the engine's update, and which switch turns which one
// off. None of that is repeated here.
//
// ONE IMPLEMENTATION FACT THAT LIVES ONLY HERE. Ayesha is built with
// incremental linking, so its calls hop through a jump table first and every
// address below is the resolved target rather than the call operand. The three
// controller offsets the ray dereferences were read out of Ayesha's own
// resolver, not carried across from the Arland build.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "field_physics.h"
#include "../../core/config.h"        // verboseLogging
#include "../../core/game.h"
#include "../../core/log.h"
#include "../../core/mem.h"
#include "../../core/protection_transaction.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using FieldUpdateProc = void (STDMETHODCALLTYPE*)(uintptr_t, float);
FieldUpdateProc originalFieldUpdate = nullptr;

// Controller offsets. Carried over from the Arland implementation, and since
// confirmed against both Ayesha builds by scanning the controller update
// (EN 0x739fa0, ML 0x75c4a0) for every access to each one. The two this file
// writes carry the strongest evidence: +0x54 is accumulated and then clamped to
// -20.0f terminal velocity, and +0xb8 is reset to zero and accumulated by frame
// time.
//
// kGroundedOffset/kGroundedBit are the one indirect pair. The object holds two
// adjacent BYTE flags at +0x38 and +0x39, not a flags word, and ground contact
// is the byte at +0x39. Reading a uint32 at +0x38 and masking 0x100 lands on
// that byte's bit 0, so it is correct as long as the byte holds 0 or 1, which
// every write site in both builds does. Left as-is rather than rewritten to a
// byte read because the mask is what the Arland side uses and the two want to
// stay comparable; if that ever stops being true, read the byte directly.
constexpr uintptr_t kGroundedOffset = 0x38;   // see above: really byte +0x39
constexpr uintptr_t kVelOffset = 0x50;        // three contiguous floats
constexpr uintptr_t kVelYOffset = 0x54;
constexpr uintptr_t kPosOffset = 0x60;
constexpr uintptr_t kEntryPosOffset = 0x70;   // pos is copied here at entry
constexpr uintptr_t kAirTimerOffset = 0xb8;
// Confirmed in Ayesha's own resolver, not assumed from Arland: it loads the
// collision wrapper from [this+0x98] and the foot offset from [this+0xb4], and
// the translate helper guards on [this+0x20] for the scene node.
constexpr uintptr_t kQueryIfaceOffset = 0x98;
constexpr uintptr_t kFootOffset = 0xb4;
constexpr uintptr_t kNodeOffset = 0x20;
constexpr uintptr_t kWrapperScene = 0x10;
constexpr uintptr_t kWrapperDirty = 0x18;
constexpr uint32_t kGroundedBit = 0x100;

// Past the last field touched here, so one range check covers the whole set.
constexpr size_t kControllerSpan = 0xbc;

// The value the games ship, compared as an exact bit pattern rather than as a
// float: 0x3c0b4396 == 0.008500000461935997. Refusing to touch anything else is
// what stops a wrong address from corrupting unrelated data.
constexpr uint32_t kShippedThresholdBits = 0x3c0b4396u;
constexpr float kShippedThreshold = 0.0085f;
constexpr float kReferenceDt = 1.0f / 60.0f;
// A floor, reached only past ~1000 fps, below which the threshold stops being
// meaningful and starts colliding with the engine's own epsilons.
constexpr float kMinThreshold = 0.0005f;

// Per-build addresses. The threshold is a float in the writable data section
// with exactly one reader -- the collision resolver, at +0x5d1 -- and no writer
// anywhere in the image. The resolver is verified before the threshold is
// trusted, since neither is meaningful without the other.
struct FieldPhysicsAddrs {
  uintptr_t update;
  uintptr_t collisionResolver;
  uintptr_t moveThreshold;
};

constexpr FieldPhysicsAddrs kAyeshaEn    { 0x739fa0, 0x738670, 0x1627f20 };
constexpr FieldPhysicsAddrs kAyeshaMulti { 0x75c4a0, 0x75ab70, 0x17bf8e0 };

const FieldPhysicsAddrs* addressesFor(uint8_t exeBuild) {
  // Ayesha only. Escha & Logy and Shallie are on KTGL, are not mapped for this
  // fix, and the capability matrix hard-offs it for them.
  return exeBuild == BuildEnglish ? &kAyeshaEn : &kAyeshaMulti;
}

float* g_moveThreshold = nullptr;   // null unless verified and made writable

// ON BY DEFAULT, through the capability matrix. The threshold address and its
// single reader are verified statically, and the fix is confirmed in game.
//
// This MUST go through featureEnabled() rather than reading the environment
// directly. Both halves once did the latter, which silently pinned them off
// after the matrix promoted them: the log reported `FieldEngineFix = on` from
// the matrix while installFieldPhysics saw false and logged
// `FIXES field_physics=off`. featureEnabled() is the only thing that knows
// about the matrix, the Unsupported hard-off, and the environment override,
// and any gate with a matrix column has to ask it rather than reimplement a
// third of it. The ground ray and the grace hold have no column and answer to
// their environment switches alone; game.h records that split.
bool engineFixEnabled() {
  return featureEnabled(Feature::FieldEngineFix);
}

// Rescale the resolver's minimum-movement threshold for this frame's duration,
// turning a per-frame distance into a constant speed (0.51 units/s). Identical
// to the shipped value at 60 fps, and clamped so a long frame never raises it
// above what the game itself uses.
void applyThreshold(float dt) {
  if (!g_moveThreshold || !(dt > 0.0f))
    return;
  float scaled = kShippedThreshold * (dt / kReferenceDt);
  if (scaled > kShippedThreshold)
    scaled = kShippedThreshold;
  if (scaled < kMinThreshold)
    scaled = kMinThreshold;
  *g_moveThreshold = scaled;
}


// A speed low enough that nothing the player is doing produces it, but not
// exactly zero, since these components come out of float arithmetic. For scale,
// the rescaled threshold discards anything under 0.51 units/s.

// There was an escape hatch here: every third of a second the character was
// released for one untouched frame, so that ground moving away underneath a
// held character would still be noticed. It was removed once it was traced
// through instead of reasoned about, because it could not do that.
//
// A released frame starts from vel.Y = 0, since the previous frame zeroed it,
// so the distance it produces is g*dt^2: about 0.0007 units at 144 Hz. That is
// below the rescaled threshold, and below the game's own, at every frame rate
// above roughly 29 fps. So the resolver reverts the released frame like any
// other and the ground-snap sweep, which sits on the other branch, still never
// runs. The hatch cost a frame of holding and bought nothing.
//
// The hazard it was aimed at also does not arise. Ground moving up into the
// character needs no hatch: penetration push-out is applied to the position
// before the movement is measured, so it accumulates until the frame stands on
// its own, the position no longer matches the entry copy, and the hold releases
// itself. Ground receding downward would need the snap, but no Ayesha field map
// has moving floors, lifts or platforms to produce it.
//
// If one is ever needed, the only form that works is to stop holding and stay
// released until a frame is not reverted, which from rest takes about five
// frames at 144 Hz and always about half the grounded grace period, at any
// frame rate, provided the rescale is active.

// The controller is a live heap object reached through a pointer the detour was
// handed, so the range is proved committed and writable before any of the
// offsets above are dereferenced. One query covers all of them, which matters
// because this runs on every field frame.
bool controllerWritable(uintptr_t self) {
  if (!self)
    return false;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!VirtualQuery(reinterpret_cast<void*>(self), &mbi, sizeof(mbi)))
    return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
    return false;
  const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & writable))
    return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  return self >= base && self + kControllerSpan <= base + mbi.RegionSize;
}

// --- the ground ray ---------------------------------------------------------
//
// THE DEFECT is in field_physics.h. What matters here is the part of it the
// header does not cover: why this casts its own ray rather than asking the
// engine for one.
//
// The engine already knows the right answer for a character standing still: it
// casts a ray down from the feet and snaps to the hit. But the gate in front of
// that demands the frame's resolved movement be under 1.1920929e-5, so anything
// moving never qualifies and takes its height from sliding along contact planes
// instead. On a slope that is what oscillates.
//
// THE CORRECTION runs the same query for a character that is moving. Ours rather
// than the engine's, for one reason: the ray length. The engine reaches 5 units
// below the feet, which is safe when the caller is already stationary on the
// ground and is not safe here -- a character that has just walked off a ledge is
// still flagged grounded for the grace period, and a 5 unit ray would snap it to
// the ground below instead of letting it fall. A short ray finds the surface
// underfoot and nothing else.
//
// Two details carry it. It runs AFTER the update, so the height is set for the
// position the character actually ends the frame at rather than the one it left;
// correcting first leaves the height stale by one frame's step, which on a slope
// reads as falling behind the terrain instead of hugging it. And on a hit the
// air timer is cleared, because the engine only notices ground when a character
// sinks into it -- one held exactly on the surface never re-establishes contact,
// the grace period expires, and it drops.
//
// Ported from the Arland games, where it was measured: residual motion falls to
// one gravity step per frame, about a hundred times smaller than the bounce it
// replaces. Every address below was read out of Ayesha's own binary rather than
// carried over, because Ayesha is built with incremental linking and its calls
// hop through a jump table first; these are the resolved targets.
struct GroundRayAddrs {
  uintptr_t raycast;
  uintptr_t filterVtable;  // PSSG::detail::RaycastFilter
  uintptr_t translate;     // pos += delta, and push to the scene node
};
constexpr GroundRayAddrs kGroundRayAyeshaEn { 0x85b930, 0x10f87d0, 0x739900 };
constexpr GroundRayAddrs kGroundRayAyeshaMl { 0x87de30, 0x123f700, 0x75be00 };

const GroundRayAddrs* groundRayAddressesFor(uint8_t exeBuild) {
  return exeBuild == BuildEnglish ? &kGroundRayAyeshaEn : &kGroundRayAyeshaMl;
}

// How far below the feet to look. Well under a character's height, so a step or
// a slope is found and a ledge is not.
constexpr float kGroundRayReach = 0.35f;
// The engine's own bias, so a character sits where the engine would put it.
constexpr float kGroundRayBias = 0.01f;
// A character launched downward by something other than gravity keeps its speed;
// by the end of a grace period gravity alone cannot have reached this.
constexpr float kGraceHoldMaxSpeed = 8.0f;

// The query descriptor, read out of Ayesha's own construction of it. Laid out as
// raw bytes with explicit offsets rather than as a struct: the engine's field
// order is what it is, and a compiler that pads differently would corrupt the
// call rather than fail to build.
constexpr size_t kRayDescSize = 0x70;
constexpr size_t kRayHitPos = 0x00;    // out, vec4
constexpr size_t kRayOrigin = 0x20;    // in, vec4
constexpr size_t kRayDirection = 0x30; // in, vec4, unit
constexpr size_t kRayFlag40 = 0x40;
constexpr size_t kRayDist44 = 0x44;
constexpr size_t kRayHitObject = 0x48; // out
constexpr size_t kRayFilter = 0x50;    // in, points at one qword: the vtable
constexpr size_t kRayMask = 0x58;      // in, the engine passes 3
constexpr size_t kRayDist60 = 0x60;    // in, the extent actually read
constexpr size_t kRayFlag64 = 0x64;
constexpr size_t kRayZero68 = 0x68;

using PFN_Raycast = void* (*)(void* scene, void* descriptor);
using PFN_Translate = void (*)(uintptr_t self, const void* delta);

bool g_groundRayActive = false;
bool g_graceActive = false;
PFN_Raycast g_raycast = nullptr;
PFN_Translate g_translate = nullptr;
uintptr_t g_filterVtable = 0;     // the whole filter object is this one pointer

struct GroundRayCounts {
  uint32_t calls;
  uint32_t notGround;
  uint32_t noScene;
  uint32_t dirty;
  uint32_t missed;
  uint32_t rejected;
  uint32_t applied;
};
GroundRayCounts g_rayCounts = {};

bool applyGroundRay(uintptr_t self) {
  if (!g_groundRayActive || !controllerWritable(self))
    return false;
  ++g_rayCounts.calls;
  // Sampled every 4000 calls rather than logged per call: at 200 Hz the ray
  // runs once a frame, so an ungated line would be most of the log.
  if (verboseLogging() && g_rayCounts.calls % 4000 == 0) {
    log("GROUNDRAY calls=", std::dec, g_rayCounts.calls,
        " applied=", g_rayCounts.applied,
        " notGround=", g_rayCounts.notGround,
        " noScene=", g_rayCounts.noScene,
        " dirty=", g_rayCounts.dirty,
        " missed=", g_rayCounts.missed,
        " rejected=", g_rayCounts.rejected);
  }

  uint32_t flags = 0;
  float pos[3] = {};
  float foot = 0.0f;
  uintptr_t wrapper = 0;
  uintptr_t node = 0;
  std::memcpy(&flags, reinterpret_cast<const void*>(self + kGroundedOffset),
              sizeof(flags));
  std::memcpy(pos, reinterpret_cast<const void*>(self + kPosOffset), sizeof(pos));
  std::memcpy(&foot, reinterpret_cast<const void*>(self + kFootOffset),
              sizeof(foot));
  std::memcpy(&wrapper, reinterpret_cast<const void*>(self + kQueryIfaceOffset),
              sizeof(wrapper));
  std::memcpy(&node, reinterpret_cast<const void*>(self + kNodeOffset),
              sizeof(node));

  // A jump clears the grounded bit outright, so this cannot fight one, and a
  // genuine fall has already cleared it by the time it matters. A rising
  // character is deliberately NOT skipped: walking up a slope reads as upward
  // velocity, and refusing those frames refuses the whole uphill walk.
  if ((flags & kGroundedBit) == 0) {
    ++g_rayCounts.notGround;
    return false;
  }
  if (!wrapper || !node) {
    ++g_rayCounts.noScene;
    return false;
  }

  uintptr_t scene = 0;
  uint8_t dirty = 1;
  if (!tryRead(wrapper + kWrapperScene, scene) ||
      !tryRead(wrapper + kWrapperDirty, dirty) || !scene) {
    ++g_rayCounts.noScene;
    return false;
  }
  // A dirty scene is rebuilt by the engine before its own queries. Rather than
  // call that rebuild, this frame is skipped: a missed correction costs one
  // frame of the defect it is fixing, and calling into a rebuild from here would
  // be the most invasive thing this feature does.
  if (dirty != 0) {
    ++g_rayCounts.dirty;
    return false;
  }

  alignas(16) uint8_t descriptor[kRayDescSize] = {};
  const float origin[4] = { pos[0], pos[1] + foot, pos[2], 0.0f };
  const float direction[4] = { 0.0f, -1.0f, 0.0f, 0.0f };
  const float reach = foot + kGroundRayReach;
  const uint64_t mask = 3;
  const uint64_t zero = 0;
  const void* filter = &g_filterVtable;
  std::memcpy(descriptor + kRayOrigin, origin, sizeof(origin));
  std::memcpy(descriptor + kRayDirection, direction, sizeof(direction));
  descriptor[kRayFlag40] = 0;
  std::memcpy(descriptor + kRayDist44, &reach, sizeof(reach));
  std::memcpy(descriptor + kRayHitObject, &zero, sizeof(zero));
  std::memcpy(descriptor + kRayFilter, &filter, sizeof(filter));
  std::memcpy(descriptor + kRayMask, &mask, sizeof(mask));
  std::memcpy(descriptor + kRayDist60, &reach, sizeof(reach));
  descriptor[kRayFlag64] = 1;
  std::memcpy(descriptor + kRayZero68, &zero, sizeof(zero));

  if (!g_raycast(reinterpret_cast<void*>(scene), descriptor)) {
    ++g_rayCounts.missed;   // nothing underfoot: a real fall, leave it alone
    return false;
  }

  float hitY = 0.0f;
  std::memcpy(&hitY, descriptor + kRayHitPos + sizeof(float), sizeof(hitY));
  const float delta = (hitY + kGroundRayBias) - pos[1];
  // The ray cannot report a surface further than its own length, so a delta
  // outside that means the hit is not what this feature thinks it is. Refusing
  // is free; a bad correction is a character teleporting.
  if (!(delta > -reach && delta < reach)) {
    ++g_rayCounts.rejected;
    return false;
  }

  const float move[4] = { 0.0f, delta, 0.0f, 0.0f };
  g_translate(self, move);
  const float rest = 0.0f;
  std::memcpy(reinterpret_cast<void*>(self + kVelYOffset), &rest, sizeof(rest));
  std::memcpy(reinterpret_cast<void*>(self + kAirTimerOffset), &rest,
              sizeof(rest));
  ++g_rayCounts.applied;
  return true;
}

// The weaker half, for the frames where the ray finds no ground: inside the
// grace period the engine still calls the character grounded, so its vertical
// velocity has no business accumulating. Removing it turns the fall from
// quadratic in the window to linear. The timer is NOT pinned here -- a character
// that really has walked off a ledge must start falling when the period expires.
bool graceHoldEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DUSK_FIELD_GRACE_HOLD");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool groundRayEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DUSK_FIELD_GROUND_RAY");
    return !value || value[0] != '0';
  }();
  return enabled;
}

void applyGraceHold(uintptr_t self) {
  if (!g_graceActive || !controllerWritable(self))
    return;
  uint32_t flags = 0;
  float airTimer = 0.0f;
  float velY = 0.0f;
  std::memcpy(&flags, reinterpret_cast<const void*>(self + kGroundedOffset),
              sizeof(flags));
  std::memcpy(&airTimer, reinterpret_cast<const void*>(self + kAirTimerOffset),
              sizeof(airTimer));
  std::memcpy(&velY, reinterpret_cast<const void*>(self + kVelYOffset),
              sizeof(velY));
  // Grounded but with the grace timer running is exactly the window: the flag
  // is still set and the contact that set it is gone.
  if ((flags & kGroundedBit) == 0 || !(airTimer > 0.0f))
    return;
  if (!(velY < 0.0f) || velY < -kGraceHoldMaxSpeed)
    return;
  const float rest = 0.0f;
  std::memcpy(reinterpret_cast<void*>(self + kVelYOffset), &rest, sizeof(rest));
}

void STDMETHODCALLTYPE tracedFieldUpdate(uintptr_t self, float dt) {
  // Before the update, so the resolver this call drives reads the value meant
  // for this frame.
  applyThreshold(dt);
  // Also before it, because the grace hold works by taking away the vertical
  // velocity the update is about to integrate.
  applyGraceHold(self);

  originalFieldUpdate(self, dt);

  // After it, so the height is set for the position the character actually ends
  // the frame at. See the note above applyGroundRay.
  applyGroundRay(self);
}

// Confirm the threshold really holds the shipped value, and make its page
// writable. The page is protected explicitly rather than assumed, and the
// original protection is retained until the hook has installed so a failed
// attempt can put the whole process state back.
bool prepareThreshold(BYTE* base, const FieldPhysicsAddrs& addrs,
                      ProtectionTransaction& protection) {
  auto* threshold = reinterpret_cast<float*>(base + addrs.moveThreshold);
  uint32_t bits = 0;
  if (!tryRead(reinterpret_cast<uintptr_t>(threshold), bits)) {
    log("FIELDPHYS EngineFix declined: threshold is not readable");
    return false;
  }
  if (bits != kShippedThresholdBits) {
    log("FIELDPHYS EngineFix declined: expected 0x", std::hex,
        kShippedThresholdBits, " at the threshold, found 0x", bits, std::dec);
    return false;
  }
  if (!protection.change(threshold, sizeof(float), PAGE_READWRITE)) {
    log("FIELDPHYS EngineFix declined: threshold page is not writable");
    return false;
  }
  g_moveThreshold = threshold;
  return true;
}

}  // namespace

bool installFieldPhysics(BYTE* base, uint8_t exeBuild) {
  const bool wantFix = engineFixEnabled();
  const bool wantGrace = graceHoldEnabled();
  const GroundRayAddrs* rayAddrs =
    groundRayEnabled() ? groundRayAddressesFor(exeBuild) : nullptr;
  if (!wantFix && !wantGrace && !rayAddrs) {
    log("FIXES field_physics=off");
    return false;
  }
  const FieldPhysicsAddrs* addrs = addressesFor(exeBuild);
  if (!addrs) {
    log("FIXES field_physics=failed (unsupported executable)");
    return false;
  }

  // One array covers all six builds for each function: no RIP displacement
  // falls inside either 16-byte window.
  const std::array<BYTE, 16> updateExpected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x60, 0x0f, 0x29,
    0x74, 0x24, 0x50, 0x48, 0x8b, 0xd9, 0x48, 0x8b,
  };
  const std::array<BYTE, 16> resolverExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xd8,
  };
  if (!matches(base + addrs->update, updateExpected)) {
    log("FIELDPHYS declined: unexpected controller-update prologue");
    return false;
  }
  // The threshold is only meaningful if the function reading it is the one we
  // think it is.
  if (wantFix &&
      !matches(base + addrs->collisionResolver, resolverExpected)) {
    log("FIELDPHYS declined: unexpected collision-resolver prologue");
    return false;
  }
  ProtectionTransaction protection;
  if (wantFix && !prepareThreshold(base, *addrs, protection))
    return false;
  g_graceActive = wantGrace;

  // The ground ray calls into the game rather than only writing to it, so both
  // entry points are checked before either is armed. A wrong address here is a
  // call through a pointer into the middle of a function.
  if (rayAddrs) {
    const std::array<BYTE, 8> raycastExpected = {
      0x48, 0x8b, 0x41, 0x20, 0x48, 0x85, 0xc0, 0x74,
    };
    const std::array<BYTE, 12> translateExpected = {
      0x48, 0x83, 0xec, 0x38, 0x48, 0x83, 0x79, 0x20,
      0x00, 0x74, 0x66, 0xf3,
    };
    if (!matches(base + rayAddrs->raycast, raycastExpected)) {
      log("FIELDPHYS ground ray declined: unexpected query prologue at 0x",
          std::hex, rayAddrs->raycast, std::dec);
    } else if (!matches(base + rayAddrs->translate, translateExpected)) {
      log("FIELDPHYS ground ray declined: unexpected translate prologue at 0x",
          std::hex, rayAddrs->translate, std::dec);
    } else {
      g_raycast = reinterpret_cast<PFN_Raycast>(base + rayAddrs->raycast);
      g_translate = reinterpret_cast<PFN_Translate>(base + rayAddrs->translate);
      g_filterVtable =
        reinterpret_cast<uintptr_t>(base + rayAddrs->filterVtable);
      g_groundRayActive = true;
    }
  }

  const bool installed = installMinHookDetour(base + addrs->update,
    reinterpret_cast<void*>(&tracedFieldUpdate),
    reinterpret_cast<void**>(&originalFieldUpdate));
  if (!installed) {
    // No frame ran through the failed hook, so the value is still the shipped
    // one; the mutation to undo is the writable page protection itself.
    const bool restored = protection.rollback();
    if (!restored)
      log("FIELDPHYS rollback_incomplete: threshold page protection could not"
          " be restored");
    else
      g_moveThreshold = nullptr;
    g_graceActive = false;
    g_groundRayActive = false;
  } else {
    protection.commit();
  }
  log("FIXES field_physics=", installed ? "active" : "failed",
      " engine_fix=", installed && g_moveThreshold ? 1 : 0,
      " grace_hold=", g_graceActive ? 1 : 0,
      " ground_ray=", g_groundRayActive ? 1 : 0);
  return installed;
}

}  // namespace atfix
