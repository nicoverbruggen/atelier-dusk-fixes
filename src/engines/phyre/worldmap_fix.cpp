// SPDX-License-Identifier: MIT
//
// See worldmap_fix.h for the defect and the correction.
//
// PROVENANCE. Ported from the Arland project's src/worldmap_fix.cpp (this
// project's own code, MIT), where the same defect was found in Totori and
// Meruru and the fix is runtime-confirmed at both 144 and 60 fps. The mechanism
// is unchanged; only the address pack and the gating differ.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
using PublishProc = void (STDMETHODCALLTYPE*)(uintptr_t, const Vec4*);

// The three functions this fix needs, per executable, plus the prologue windows
// that gate them. An empty table is the honest state for a build whose
// addresses have not been derived: installWorldMapFix declines and says so,
// rather than hooking an address that means something else.
struct WorldMapAddrs {
  uintptr_t driver;    // receives the real frame dt and calls the mover
  uintptr_t move;      // the mover: adds a normalized vector to the position
  uintptr_t publish;   // pushes the position to the render object
  std::array<BYTE, 16> driverExpected;
  std::array<BYTE, 16> moveExpected;
  std::array<BYTE, 16> publishExpected;
};

// Filled once the homologs are derived for both Ayesha builds. Until then this
// subsystem installs nothing, which is the correct behaviour and not a stub:
// every other fix in this directory is gated on a verified prologue for exactly
// the same reason.
const WorldMapAddrs* addressesFor(uint8_t /*exeBuild*/) {
  return nullptr;
}

// Struct offsets, to be confirmed against Ayesha rather than assumed. In the
// Arland movers the authoritative position is at self+0x30 and the render
// object it is published to is at [self+0x28]; the same engine lineage makes
// those the first candidates, not the answer.
constexpr uintptr_t kPositionOffset = 0x30;
constexpr uintptr_t kRenderObjectOffset = 0x28;

DriverProc originalDriver = nullptr;
MoveProc originalMove = nullptr;
PublishProc publishPosition = nullptr;
const WorldMapAddrs* g_addrs = nullptr;

// The mover is reached from the driver, so the driver's dt belongs to this call
// chain rather than to the process. thread_local keeps two threads from handing
// each other a delta time that was never theirs.
thread_local float g_updateDt = 0.0f;

std::atomic<uint32_t> g_moveCalls{0};
std::atomic<uint32_t> g_lines{0};
constexpr uint32_t kMaxLines = 600;

bool fixEnabled() {
  return featureEnabled(Feature::WorldMapCursor);
}

bool probeEnabled() {
  const char* value = std::getenv("DUSK_WORLDMAP_PROBE");
  return value && value[0] != '0';
}

float distance3(const Vec4& a, const Vec4& b) {
  const float x = b[0] - a[0];
  const float y = b[1] - a[1];
  const float z = b[2] - a[2];
  return std::sqrt(x * x + y * y + z * z);
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
  float appliedStep = rawStep;
  float factor = 1.0f;

  if (fixEnabled() && haveBefore && haveAfter && rawStep > 0.0f && dt > 0.0f) {
    // min(dt * 60, 1): at 60 fps and below this is 1 and the shipped behaviour
    // is bit-for-bit preserved. Clamping rather than scaling freely matters --
    // a long frame (a load, a breakpoint) would otherwise teleport the cursor.
    factor = std::clamp(dt * 60.0f, 0.0f, 1.0f);
    Vec4 corrected = after;
    for (size_t i = 0; i < 3; ++i)
      corrected[i] = before[i] + (after[i] - before[i]) * factor;

    // Both, not just the first. The mover owns the authoritative position and
    // publishes it to the render object; correcting only its own copy would
    // leave the two disagreeing for a frame, and correcting only the render
    // copy would be overwritten on the next integration.
    std::memcpy(reinterpret_cast<void*>(self + kPositionOffset),
                corrected.data(), sizeof(corrected));
    uintptr_t target = 0;
    if (tryRead(self + kRenderObjectOffset, target) && target && publishPosition)
      publishPosition(target, &corrected);
    after = corrected;
    appliedStep = distance3(before, after);
  }

  if (probeEnabled()) {
    const uint32_t call =
      g_moveCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call == 1)
      log("WMPROBE mover FIRED rva=0x", std::hex, g_addrs ? g_addrs->move : 0,
          std::dec, " self=", reinterpret_cast<void*>(self), " dt=", dt,
          " fix=", fixEnabled());
    // raw_per_s against applied_per_s is the whole measurement: the first
    // should scale with refresh rate and the second should not.
    if (rawStep > 1e-6f &&
        g_lines.fetch_add(1, std::memory_order_relaxed) < kMaxLines)
      log("WMPROBE move n=", call, " dt=", dt,
          " raw_step=", rawStep, " factor=", factor,
          " applied_step=", appliedStep,
          " raw_per_s=", (dt > 0.0f) ? rawStep / dt : 0.0f,
          " applied_per_s=", (dt > 0.0f) ? appliedStep / dt : 0.0f);
  }
  return result;
}

}  // namespace
}  // namespace atfix

namespace dusk {

bool installWorldMapFix(BYTE* base, uint8_t exeBuild) {
  using namespace atfix;
  auto& log = atfix::log;   // std::log is also in scope via <cmath>
  const bool diagnostic = probeEnabled();
  if (!fixEnabled() && !diagnostic) {
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
  auto* publish = base + g_addrs->publish;
  if (!matches(driver, g_addrs->driverExpected) ||
      !matches(move, g_addrs->moveExpected) ||
      !matches(publish, g_addrs->publishExpected)) {
    log("FIXES world_map=declined (prologue mismatch)");
    return false;
  }
  publishPosition = reinterpret_cast<PublishProc>(publish);

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
      " move_rva=0x", g_addrs->move, std::dec,
      " probe=", diagnostic ? 1 : 0);
  return moveOk && driverOk;
}

}  // namespace dusk
