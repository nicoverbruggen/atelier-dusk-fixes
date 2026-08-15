// SPDX-License-Identifier: MIT
//
// See worldmap_fix.h for the defect and the correction.
//
// PROVENANCE. Ported from this project's own src/engines/phyre/worldmap_fix.cpp
// (MIT), itself ported from the Arland project, where the same defect is
// runtime-confirmed in Totori and Meruru at both 144 and 60 fps. The mechanism
// is unchanged. The address pack, the offsets and the publish differ.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "worldmap_fix.h"
#include "ktgl.h"
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

struct WorldMapAddrs {
  uintptr_t driver;   // receives the real frame dt and calls the mover
  uintptr_t move;     // the mover: adds a per-frame step to the position
};

// WMStateAutoMove's sub-state update callback and the cursor mover it calls.
//
// DERIVATION. The class is named by the game: the constructor at EN 0x398310
// installs vtable 0x94c748, which RTTI names WMStateAutoMove, and `lea`s the
// driver as the first registered {enter, update} pair. The mover is reached by
// the single static call from the driver. Neither appears in a vtable or at any
// other call site, so the constructor's `lea` is the only reference to them --
// which is why a call-site search alone finds nothing.
//
// The dispatcher passes dt in xmm1 to every registered callback, so the driver
// holds the real frame delta; it simply never forwards it. The mover takes rcx
// only and reads no incoming XMM register at all.
constexpr WorldMapAddrs kEschaEn    { 0x398bc0, 0x399ec0 };
constexpr WorldMapAddrs kEschaMulti { 0x3b9c90, 0x3baf90 };

// Both prologue windows are byte-identical across the two Escha builds, so
// there is one pair rather than one per row. Verified against all four
// executables before this pack was written.
constexpr std::array<BYTE, 16> kDriverExpected = {
  0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x41, 0x56,
  0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1, 0x48, 0x81,
};
constexpr std::array<BYTE, 16> kMoveExpected = {
  0x40, 0x55, 0x53, 0x48, 0x8d, 0x6c, 0x24, 0xb1,
  0x48, 0x81, 0xec, 0xe8, 0x00, 0x00, 0x00, 0x80,
};

// The cursor position on WMStateAutoMove: a 16-byte-aligned float[4] the mover
// reads and writes whole. Only the first three lanes carry the cursor.
constexpr uintptr_t kPositionOffset = 0x20;

// The render node, and Escha inlines its publish inside the mover exactly as
// Ayesha does, so there is no helper to call and the correction reproduces the
// stores itself. They are the engine's standard {previous, target, current,
// timer} interpolator block; writing all three vectors with timer = 0 is how
// the engine spells "snapped, not interpolating", which is what the mover
// itself does. So this changes the values and not the state.
//
// The node is four dereferences away, which is three more than Ayesha needs.
// Every step is checked: a chain this deep is the one part of this file that
// can fault, and a missed check would turn a cursor-speed fix into a crash.
constexpr uintptr_t kOwnerOffset = 0x8;
constexpr uintptr_t kOwnerOwnerOffset = 0x8;
constexpr uintptr_t kManagerOffset = 0x38;
constexpr uintptr_t kNodeOffset = 0x2b8;
constexpr uintptr_t kNodePrevious = 0x98;
constexpr uintptr_t kNodeTarget = 0xa8;
constexpr uintptr_t kNodeCurrent = 0xb8;
constexpr uintptr_t kNodeTimer = 0xc8;
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
  return featureEnabled(Feature::WorldMapCursor);
}

float distance3(const Vec4& a, const Vec4& b) {
  const float x = b[0] - a[0];
  const float y = b[1] - a[1];
  const float z = b[2] - a[2];
  return std::sqrt(x * x + y * y + z * z);
}

// Walks the chain to the render node, or returns 0. Each link is read through
// tryRead, so a null or unmapped pointer anywhere stops the walk instead of
// faulting.
uintptr_t renderNode(uintptr_t self) {
  uintptr_t owner = 0;
  if (!tryRead(self + kOwnerOffset, owner) || !owner)
    return 0;
  uintptr_t ownerOwner = 0;
  if (!tryRead(owner + kOwnerOwnerOffset, ownerOwner) || !ownerOwner)
    return 0;
  uintptr_t manager = 0;
  if (!tryRead(ownerOwner + kManagerOffset, manager) || !manager)
    return 0;
  uintptr_t node = 0;
  if (!tryRead(manager + kNodeOffset, node) || !node)
    return 0;
  return node;
}

// One range check covers previous, target, current and the timer together, so a
// stale node pointer cannot leave the block half-written.
void republish(uintptr_t self, const Vec4& position) {
  const uintptr_t node = renderNode(self);
  if (!node || !writableRange(node + kNodePrevious, kNodeBlockSpan))
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

  // rawStep > 0 also stands in for the mover's return value: with the stick at
  // rest and no direction held it returns early without touching the position,
  // and there is then nothing to rescale.
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

const WorldMapAddrs* addressesFor(const KtglGame& game) {
  // Escha & Logy only. Shallie's mover already multiplies by the frame delta --
  // see worldmap_fix.h -- so hooking it there would rescale a rate that is
  // already correct. Keyed on the executable name rather than on the build
  // byte, because the build byte only says which language pack this is.
  if (!game.executable || !std::strstr(game.executable, "Escha"))
    return nullptr;
  return game.exeBuild == BuildEnglish ? &kEschaEn : &kEschaMulti;
}

}  // namespace

bool installKtglWorldMapFix(BYTE* base, const KtglGame& game) {
  auto& log = atfix::log;   // std::log is also in scope via <cmath>
  if (!fixEnabled()) {
    log("FIXES world_map=off");
    return false;
  }

  g_addrs = addressesFor(game);
  if (!g_addrs) {
    log("FIXES world_map=unavailable (this executable has no address row;"
        " Shallie's mover already scales by frame time and needs none)");
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

}  // namespace atfix
