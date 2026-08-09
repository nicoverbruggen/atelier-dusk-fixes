// SPDX-License-Identifier: MIT
//
// Ayesha field-jitter correction: the threshold rescale and the resting
// stabilizer. See field_physics.h for the defect and how the two halves couple.
//
// Both ship ON BY DEFAULT and are confirmed in game. An early test reported that
// they did not help and the pair was held opt-in on that basis; a later session
// with BOTH switches on confirmed that they do. That earlier negative is worth
// remembering rather than forgetting, because the most likely reading of it is a
// configuration result rather than a code one -- the stabilizer refuses to run
// without the rescale, so a test that enabled only one of them, or that relied
// on an ini key while an environment variable pinned it off, would report
// exactly that failure. `DUSK_FIELD_ENGINE_FIX=0` stands the pair down for a
// comparison and `DUSK_FIELD_STABILIZER=0` disables only the second half.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "field_physics.h"
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
constexpr uintptr_t kPosYOffset = 0x64;
constexpr uintptr_t kEntryPosOffset = 0x70;   // pos is copied here at entry
constexpr uintptr_t kEntryPosYOffset = 0x74;
constexpr uintptr_t kFootYOffset = 0xb0;
constexpr uintptr_t kAirTimerOffset = 0xb8;
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
// with exactly one reader — the collision resolver, at +0x5d1 — and no writer
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
// and every gate in this tree has to ask it rather than reimplement a third of
// it.
bool engineFixEnabled() {
  return featureEnabled(Feature::FieldEngineFix);
}

// ON BY DEFAULT, and the half that writes into live game state: unlike the
// rescale, which writes one verified constant, the stabilizer writes into the
// CONTROLLER OBJECT at kVelYOffset and kAirTimerOffset. Those offsets came from
// the Arland builds and have since been confirmed against both Ayesha builds
// (see the comment on the offset constants above), which is what allowed this
// to ship on rather than as an investigation switch.
//
// Same rule as engineFixEnabled: ask featureEnabled(), never getenv.
bool stabilizerEnabled() {
  return featureEnabled(Feature::FieldStabilizer);
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

bool g_stabilizerActive = false;   // false unless requested and verified
bool g_stabilizerHeld = false;     // whether the last frame was actually held

// A speed low enough that nothing the player is doing produces it, but not
// exactly zero, since these components come out of float arithmetic. For scale,
// the rescaled threshold discards anything under 0.51 units/s.
constexpr float kRestSpeedEpsilon = 0.001f;

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
// itself. Ground receding downward would need the snap, but no Arland field map
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

// Hold the character still while it is genuinely at rest, which is what the
// rescale on its own cannot do: gravity keeps integrating against a surface, so
// a frame still breaks through every few frames and leaves a sawtooth.
//
// Rest is three conditions at once: ground contact, no horizontal velocity, and
// a previous frame whose move the resolver threw away, which shows as the
// position sitting exactly on the copy Update took on entry. The response is to
// drop the vertical velocity gravity has been accumulating and to pin the air
// timer. Pinning the timer is the load-bearing half: held at zero the grounded
// grace period can never expire, so contact does not need a breakthrough frame
// to be re-latched. Zeroing vel.Y without it makes the jitter worse rather than
// better, because that velocity ramp is the only thing that ever clears the
// threshold.
//
// This must run BEFORE the original Update, not after it. Update refreshes the
// entry-position copy on the way in, so the same test applied after the call
// compares a value against itself, always passes, and describes nothing.
void applyRestingStabilizer(uintptr_t self, float dt) {
  g_stabilizerHeld = false;
  if (!g_stabilizerActive || !(dt > 0.0f) || !controllerWritable(self))
    return;

  uint32_t flags = 0;
  float vel[3] = {};
  float pos[3] = {};
  float entryPos[3] = {};
  std::memcpy(&flags, reinterpret_cast<const void*>(self + kGroundedOffset),
              sizeof(flags));
  std::memcpy(vel, reinterpret_cast<const void*>(self + kVelOffset), sizeof(vel));
  std::memcpy(pos, reinterpret_cast<const void*>(self + kPosOffset), sizeof(pos));
  std::memcpy(entryPos, reinterpret_cast<const void*>(self + kEntryPosOffset),
              sizeof(entryPos));

  const bool grounded = (flags & kGroundedBit) != 0;
  const bool horizontallyStill = std::fabs(vel[0]) < kRestSpeedEpsilon &&
                                 std::fabs(vel[2]) < kRestSpeedEpsilon;
  // The revert copies the entry vector back verbatim, so this is an exact
  // match rather than a near one, and a single moved component disqualifies it.
  const bool moveWasReverted = pos[0] == entryPos[0] && pos[1] == entryPos[1] &&
                               pos[2] == entryPos[2];
  if (!grounded || !horizontallyStill || !moveWasReverted)
    return;

  const float zero = 0.0f;
  std::memcpy(reinterpret_cast<void*>(self + kVelYOffset), &zero, sizeof(zero));
  std::memcpy(reinterpret_cast<void*>(self + kAirTimerOffset), &zero,
              sizeof(zero));
  g_stabilizerHeld = true;
}

struct ControllerState {
  float posY = 0.0f;
  float velY = 0.0f;
  float entryPosY = 0.0f;
  float footY = 0.0f;
  uint32_t grounded = 0;
  bool valid = false;
};

ControllerState readState(uintptr_t self) {
  ControllerState state;
  state.valid = tryRead(self + kPosYOffset, state.posY) &&
                tryRead(self + kVelYOffset, state.velY) &&
                tryRead(self + kGroundedOffset, state.grounded);
  if (!state.valid)
    return state;
  tryRead(self + kEntryPosYOffset, state.entryPosY);
  tryRead(self + kFootYOffset, state.footY);
  return state;
}

// A short ring of recent frames, dumped around each contact change so the event
// is readable instead of buried in per-frame noise.
struct Frame {
  float dt = 0.0f;
  ControllerState before;
  ControllerState after;
  bool stabilized = false;
  bool used = false;
};

constexpr size_t kRing = 6;
constexpr uint32_t kMaxWindows = 8;
Frame g_ring[kRing];
size_t g_ringHead = 0;
uint32_t g_windows = 0;
uint32_t g_pendingAfter = 0;
uint32_t g_frameIndex = 0;

void emitFrame(const Frame& f, const char* tag, uint32_t index) {
  log("FIELDPHYS ", tag, " n=", std::dec, index,
      " dt=", f.dt,
      " y=", f.before.posY, "->", f.after.posY,
      " entry_y=", f.after.entryPosY,
      " vy=", f.before.velY, "->", f.after.velY,
      " flags=0x", std::hex, f.before.grounded, "->0x", f.after.grounded,
      std::dec,
      " foot=", f.after.footY,
      " threshold=", g_moveThreshold ? *g_moveThreshold : kShippedThreshold,
      " held=", f.stabilized ? 1 : 0);
}

void traceFrame(float dt, const ControllerState& before,
                const ControllerState& after) {
  if (!before.valid || !after.valid)
    return;
  const uint32_t index = ++g_frameIndex;
  Frame frame;
  frame.dt = dt;
  frame.before = before;
  frame.after = after;
  frame.stabilized = g_stabilizerHeld;
  frame.used = true;

  if (g_pendingAfter) {
    --g_pendingAfter;
    emitFrame(frame, "post ", index);
    return;
  }
  const bool contactChanged =
    ((before.grounded ^ after.grounded) & kGroundedBit) != 0;
  if (contactChanged && g_windows < kMaxWindows) {
    ++g_windows;
    log("FIELDPHYS --- contact ",
        (after.grounded & kGroundedBit) ? "GAINED" : "LOST",
        " (window ", std::dec, g_windows, " of ", kMaxWindows, ") ---");
    for (size_t i = 0; i < kRing; ++i) {
      const Frame& past = g_ring[(g_ringHead + i) % kRing];
      if (past.used)
        emitFrame(past, "pre  ", 0);
    }
    emitFrame(frame, "AT   ", index);
    g_pendingAfter = kRing;
    return;
  }
  g_ring[g_ringHead] = frame;
  g_ringHead = (g_ringHead + 1) % kRing;
}

void STDMETHODCALLTYPE tracedFieldUpdate(uintptr_t self, float dt) {
  const bool tracing = fieldTraceEnabled();
  // Snapshot first, so the trace shows the state the stabilizer judged rather
  // than the state it left behind.
  const ControllerState before = tracing ? readState(self) : ControllerState{};

  // Both of these belong before the update, for different reasons: the threshold
  // so the resolver this call drives reads the value meant for this frame, the
  // stabilizer because Update overwrites the entry-position copy it tests.
  applyThreshold(dt);
  applyRestingStabilizer(self, dt);

  originalFieldUpdate(self, dt);
  if (tracing)
    traceFrame(dt, before, readState(self));
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

bool fieldTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DUSK_FIELD_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool installFieldPhysics(BYTE* base, uint8_t exeBuild) {
  const bool wantFix = engineFixEnabled();
  // The stabilizer holds the character only while it is grounded, and without
  // the rescale that precondition can drop while the character is still
  // settling: above roughly 115 fps the grace period expires before a frame
  // moves far enough to re-establish contact. Holding the two apart is only
  // useful for an A/B, and this half of the A/B is not sound, so it is refused
  // rather than run.
  bool wantStabilizer = stabilizerEnabled();
  if (wantStabilizer && !wantFix) {
    log("FIELDPHYS stabilizer needs the threshold rescale; leaving it off");
    wantStabilizer = false;
  }
  if (!wantFix && !wantStabilizer && !fieldTraceEnabled()) {
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
  // think it is, and the stabilizer's whole premise is that same function's
  // revert, so the resolver is verified before either is used.
  if ((wantFix || wantStabilizer) &&
      !matches(base + addrs->collisionResolver, resolverExpected)) {
    log("FIELDPHYS declined: unexpected collision-resolver prologue");
    return false;
  }
  ProtectionTransaction protection;
  if (wantFix && !prepareThreshold(base, *addrs, protection))
    return false;
  g_stabilizerActive = wantStabilizer;

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
    g_stabilizerActive = false;
  } else {
    protection.commit();
  }
  log("FIXES field_physics=", installed ? "active" : "failed",
      " engine_fix=", installed && g_moveThreshold ? 1 : 0,
      " stabilizer=", g_stabilizerActive ? 1 : 0);
  log("DIAGNOSTICS field_trace=", fieldTraceEnabled() ? 1 : 0);
  return installed;
}

}  // namespace atfix
