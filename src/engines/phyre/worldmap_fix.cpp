// SPDX-License-Identifier: MIT
//
// See worldmap_fix.h for the defect and the correction.
//
// PROVENANCE. Ported from the Arland project's src/engines/phyre/worldmap_fix.cpp (this
// project's own code, MIT), where the same defect was found in Totori and
// Meruru and the fix is runtime-confirmed at both 144 and 60 fps. The mechanism
// is unchanged; the address pack, the gating, and the publish differ -- see
// "Ayesha inlines the publish" below.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "worldmap_fix.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/mem.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using Vec4 = std::array<float, 4>;
using DriverProc = bool (STDMETHODCALLTYPE*)(uintptr_t, float);
using MoveProc = bool (STDMETHODCALLTYPE*)(uintptr_t);

// The two functions this fix needs, per executable. A null row is the honest
// state for a build whose addresses have not been derived: installWorldMapFix
// declines and says so, rather than hooking an address that means something
// else.
struct WorldMapAddrs {
  uintptr_t driver;   // receives the real frame dt and calls the mover
  uintptr_t move;     // the mover: adds a per-frame step to the position
};

// WMStateAutoMove's sub-state update callback and the cursor mover it calls.
//
// DERIVATION. The class was identified by RTTI -- the WMState* family -- and
// the chain confirmed statically:
//
//   WMGameMode::Update -> WMStateMgr::Update -> WMStateAutoMove::Update
//     -> sub-state dispatch -> driver(owner, dt) -> move(self)
//
// The dispatcher passes dt in xmm1 to every registered callback, so the driver
// holds the real frame delta; it simply never forwards it. Identity is proven
// rather than inferred: the driver's thunk is `lea`'d inside the constructor
// (EN 0x31fef0), which installs vtable 0xd46e60 -- RTTI complete-object locator
// 0x114c7f0, class WMStateAutoMove -- and zeroes the +0x120 input lock that both
// driver and mover gate on. Note the sibling WMStateNormal (vtable 0xd46e08) is
// a DIFFERENT class with a different layout; an earlier pass named it here by
// mistake.
constexpr WorldMapAddrs kAyeshaEn    { 0x330c40, 0x3376a0 };
constexpr WorldMapAddrs kAyeshaMulti { 0x33e770, 0x345470 };

const WorldMapAddrs* addressesFor(uint8_t exeBuild) {
  // Ayesha only. Escha & Logy and Shallie are on KTGL, their travel map has not
  // been looked at, and the capability matrix hard-offs this for them.
  return exeBuild == BuildEnglish ? &kAyeshaEn : &kAyeshaMulti;
}

// Both prologue windows are byte-identical across the two Ayesha builds, so
// there is one pair rather than one per row. The mover's window ends inside the
// `cmp byte [rcx+0x120], 0` input-lock test, which is what makes it distinctive.
constexpr std::array<BYTE, 16> kDriverExpected = {
  0x48, 0x8b, 0xc4, 0x55, 0x57, 0x41, 0x56, 0x48,
  0x8d, 0x68, 0xa1, 0x48, 0x81, 0xec, 0x90, 0x00,
};
constexpr std::array<BYTE, 16> kMoveExpected = {
  0x40, 0x53, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00,
  0x00, 0x80, 0xb9, 0x20, 0x01, 0x00, 0x00, 0x00,
};

// WMStateNormal. The position is a 16-byte-aligned float[4] the mover reads and
// writes with movaps; only the first three lanes carry the cursor, and the
// fourth is added to with zero on every pass.
constexpr uintptr_t kPositionOffset = 0x30;
constexpr uintptr_t kRenderObjectOffset = 0x28;

// The render node reached through +0x28. Ayesha INLINES the publish inside the
// mover (EN 0x3378ea..0x33794c) instead of calling a helper the way the Arland
// movers do, so there is no PublishProc to call and the correction has to
// reproduce those four stores itself. They are the engine's standard
// {previous, target, current, timer} interpolator block, and writing all three
// vectors with timer = 0 is how the engine spells "snapped, not interpolating"
// -- exactly what the mover does, so this changes the values and not the state.
constexpr uintptr_t kNodePrevious = 0xb0;
constexpr uintptr_t kNodeTarget = 0xc0;
constexpr uintptr_t kNodeCurrent = 0xd0;
constexpr uintptr_t kNodeTimer = 0xe0;
constexpr size_t kNodeBlockSpan = kNodeTimer + sizeof(uint32_t) - kNodePrevious;
constexpr size_t kVec3Bytes = 3 * sizeof(float);

DriverProc originalDriver = nullptr;
MoveProc originalMove = nullptr;
const WorldMapAddrs* g_addrs = nullptr;

// The mover is reached from the driver, so the driver's dt belongs to this call
// chain rather than to the process. thread_local keeps two threads from handing
// each other a delta time that was never theirs.
thread_local float g_updateDt = 0.0f;

bool fixEnabled() {
  // Resolved once, as in the Arland original this was ported from. The mover
  // runs on the engine thread every frame the world map is up, and hooks on
  // that thread must not touch the ini or the environment; the rule is stated
  // in logo_skip.cpp. featureEnabled() reaches both.
  static const bool enabled = featureEnabled(Feature::WorldMapCursor);
  return enabled;
}

float distance3(const Vec4& a, const Vec4& b) {
  const float x = b[0] - a[0];
  const float y = b[1] - a[1];
  const float z = b[2] - a[2];
  return std::sqrt(x * x + y * y + z * z);
}

// Reproduces the mover's own inlined publish with the corrected position. One
// range check covers previous, target, current and the timer together, so a
// stale node pointer cannot leave the block half-written.
void republish(uintptr_t self, const Vec4& position) {
  uintptr_t node = 0;
  if (!tryRead(self + kRenderObjectOffset, node) || !node)
    return;
  if (!readableRange(node + kNodePrevious, kNodeBlockSpan))
    return;
  for (uintptr_t field : { kNodePrevious, kNodeTarget, kNodeCurrent })
    std::memcpy(reinterpret_cast<void*>(node + field), position.data(),
                kVec3Bytes);
  const uint32_t snapped = 0;
  std::memcpy(reinterpret_cast<void*>(node + kNodeTimer), &snapped,
              sizeof(snapped));
}

bool STDMETHODCALLTYPE tracedDriver(uintptr_t self, float dt) {
  const float previousDt = g_updateDt;
  g_updateDt = dt;
  const bool result = originalDriver(self, dt);
  g_updateDt = previousDt;
  return result;
}

bool STDMETHODCALLTYPE tracedMove(uintptr_t self) {
  Vec4 before{};
  const bool haveBefore = tryRead(self + kPositionOffset, before);
  const float dt = g_updateDt;

  const bool result = originalMove(self);

  Vec4 after{};
  const bool haveAfter = tryRead(self + kPositionOffset, after);
  const float rawStep =
    (haveBefore && haveAfter) ? distance3(before, after) : 0.0f;

  // rawStep > 0 is also what stands in for the mover's return value: with the
  // stick at rest and no direction held it returns early without touching the
  // position, and there is then nothing to rescale.
  if (fixEnabled() && haveBefore && haveAfter && rawStep > 0.0f && dt > 0.0f) {
    // min(dt * 60, 1): at 60 fps and below this is 1 and the shipped behaviour
    // is bit-for-bit preserved. Clamping rather than scaling freely matters --
    // a long frame (a load, a breakpoint) would otherwise teleport the cursor.
    const float factor = std::clamp(dt * 60.0f, 0.0f, 1.0f);
    Vec4 corrected = after;
    for (size_t i = 0; i < 3; ++i)
      corrected[i] = before[i] + (after[i] - before[i]) * factor;

    // Interpolating back toward `before` cannot escape the map bounds the mover
    // just clamped `after` into: both ends are inside an axis-aligned box, and
    // the box is convex.
    //
    // Both copies, not just the first. The mover owns the authoritative
    // position and publishes it to the render node; correcting only its own
    // copy would leave the two disagreeing for a frame, and correcting only the
    // node would be overwritten on the next integration.
    std::memcpy(reinterpret_cast<void*>(self + kPositionOffset),
                corrected.data(), kVec3Bytes);
    republish(self, corrected);
  }
  return result;
}

}  // namespace
}  // namespace atfix

namespace dusk {

bool installWorldMapFix(BYTE* base, uint8_t exeBuild) {
  using namespace atfix;
  auto& log = atfix::log;   // std::log is also in scope via <cmath>
  if (!fixEnabled()) {
    log("FIXES world_map=off");
    return false;
  }

  g_addrs = addressesFor(exeBuild);
  if (!g_addrs) {
    log("FIXES world_map=unavailable (no address row for this executable;"
        " the travel-map functions have not been derived for this build)");
    return false;
  }

  auto* driver = base + g_addrs->driver;
  auto* move = base + g_addrs->move;
  if (!matches(driver, kDriverExpected) || !matches(move, kMoveExpected)) {
    log("FIXES world_map=declined (prologue mismatch)");
    return false;
  }

  // The mover goes in first on purpose. Without the driver's dt it corrects
  // nothing -- the guard requires dt > 0 -- so a half-installed pair leaves the
  // game exactly as it shipped rather than scaling by a stale delta.
  const bool moveOk = installMinHookDetour(move,
    reinterpret_cast<void*>(&tracedMove),
    reinterpret_cast<void**>(&originalMove));
  const bool driverOk = moveOk && installMinHookDetour(driver,
    reinterpret_cast<void*>(&tracedDriver),
    reinterpret_cast<void**>(&originalDriver));

  log("FIXES world_map=", moveOk && driverOk ? "active" : "failed",
      " driver_rva=0x", std::hex, g_addrs->driver,
      " move_rva=0x", g_addrs->move, std::dec);
  return moveOk && driverOk;
}

}  // namespace dusk
