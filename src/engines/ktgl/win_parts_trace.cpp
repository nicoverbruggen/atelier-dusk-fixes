// SPDX-License-Identifier: MIT
//
// `DUSK_WINPARTS_TRACE`: where Escha & Logy or Shallie puts each interface part
// in its 1920x1080 canvas, and whether those positions are whole numbers. A
// diagnostic. It changes nothing the game does, and it is off unless the switch
// is set.
//
// THE QUESTION IT ANSWERS. Escha's menu text looks worse than Shallie's at
// 2560x1440 and identical at 1920x1080, and a static read of both executables
// found exactly one difference on the geometry path: Escha's `WinParts::update`
// truncates the part's canvas translation to integers and Shallie's does not.
// Escha at `0x3701f6` runs `cvttss2si` then `cvtdq2ps` on x and y before storing
// them; the same place in Shallie's `0x298eb0` stores what it was given. It is a
// floor, not a round to nearest -- there is no `+0.5`.
//
// That is a difference in the code. Whether it is the difference the player sees
// is a separate claim, and this trace exists because two earlier branches of the
// same investigation -- the texture sampler, and the art assets -- each produced
// a confident wrong answer from static reading alone.
//
// WHAT A CLEAN RESULT LOOKS LIKE. Escha reports every position integral and
// Shallie reports most of them fractional. Then the static read is confirmed and
// the mechanism is real.
//
// The trace also settles a cheaper question in the same run, and it is the one
// that would invalidate everything above: whether Escha's main menu goes through
// `WinParts` at all. If the counters stay at zero while the menu is on screen,
// it does not, and the difference found statically is in a path nobody looks at.
// A hook that never fires is a result, so the summary prints either way.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "win_parts_trace.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/util.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// One argument, and that is checked rather than assumed: both builds run
// `mov rbx, rcx` as the fourth instruction and reach their first call without
// reading rdx, r8, r9 or any xmm argument register. The return value belongs to
// whatever the body ends on, so the trace declares one and hands it back rather
// than discarding a register it does not own.
using WinPartsUpdateProc = uintptr_t (STDMETHODCALLTYPE*)(uintptr_t);

WinPartsUpdateProc originalWinPartsUpdate = nullptr;

std::atomic<bool> tracing{false};
std::atomic<uint32_t> nodeOffset{0};
std::atomic<uint32_t> translateOffset{0};

std::atomic<uint64_t> calls{0};
std::atomic<uint64_t> integral{0};
std::atomic<uint64_t> fractional{0};

// One line per distinct position, capped. These menus rebuild every frame, so an
// uncapped log would be a gigabyte of the same twenty numbers.
constexpr int kSampleLines = 24;
std::atomic<int> samplesLeft{kSampleLines};

// Every 600 calls, which is roughly a second of a menu that rebuilds each frame.
// The tally is what actually answers the question; the samples are there so the
// numbers can be read rather than trusted.
constexpr uint64_t kSummaryEvery = 600;

// A position is "integral" when it is exactly a whole number. Exactly, not
// nearly: the claim under test is that Escha truncates, and a truncated float is
// bit-exact whole. A tolerance here would let a value that merely landed close
// count as evidence of a floor that is not there.
bool isWhole(float v) {
  return std::floor(v) == v;
}

uintptr_t STDMETHODCALLTYPE tracedWinPartsUpdate(uintptr_t self) {
  const uintptr_t result = originalWinPartsUpdate(self);
  if (!tracing.load(std::memory_order_relaxed) || !self)
    return result;

  // AFTER the original, because the translation this reads is what that call
  // just wrote. Reading it first would sample the previous frame's value and the
  // trace would still look plausible.
  const uintptr_t node =
    *reinterpret_cast<uintptr_t*>(self + nodeOffset.load(std::memory_order_relaxed));
  if (!node)
    return result;
  const float* translate =
    reinterpret_cast<const float*>(node + translateOffset.load(std::memory_order_relaxed));
  const float x = translate[0];
  const float y = translate[1];

  const uint64_t seen = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool whole = isWhole(x) && isWhole(y);
  if (whole)
    integral.fetch_add(1, std::memory_order_relaxed);
  else
    fractional.fetch_add(1, std::memory_order_relaxed);

  // Only fractional positions are sampled once the first few have gone by. An
  // integral list proves nothing beyond its tally, while the fractional values
  // are the ones worth reading: their size says whether the difference is a
  // sub-pixel phase or a whole part being placed somewhere else.
  if (samplesLeft.load(std::memory_order_relaxed) > 0 &&
      (!whole || seen <= 4) &&
      samplesLeft.fetch_sub(1, std::memory_order_relaxed) > 0)
    log("WINPARTS x=", x, " y=", y, whole ? " (whole)" : " (fractional)");

  if (seen % kSummaryEvery == 0)
    log("WINPARTS calls=", std::dec, seen,
        " whole=", integral.load(std::memory_order_relaxed),
        " fractional=", fractional.load(std::memory_order_relaxed));
  return result;
}

// `mov [rsp+8], rbx / push rbp / mov rbp, rsp / sub rsp, 0x50 / movaps [rsp+0x40], xmm6`.
// Byte-identical in both English builds, which is expected -- this is one
// function shared by the six WinParts classes and the two games compiled it the
// same way. The window ends mid-instruction, which is fine for verification.
constexpr std::array<BYTE, 16> kWinPartsUpdateExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x08, 0x55, 0x48, 0x8b,
  0xec, 0x48, 0x83, 0xec, 0x50, 0x0f, 0x29, 0x74
};

}  // namespace

bool installWinPartsTrace(BYTE* base, const KtglGame& game) {
  const char* enabled = std::getenv("DUSK_WINPARTS_TRACE");
  if (!enabled || enabled[0] == '0')
    return false;
  if (!game.winPartsUpdateRva || !game.winPartsTranslateOffset) {
    log("WINPARTS trace: no address row for this build");
    return false;
  }

  BYTE* target = base + game.winPartsUpdateRva;
  if (!matches(target, kWinPartsUpdateExpected)) {
    log("WINPARTS trace: prologue mismatch at 0x", std::hex,
        game.winPartsUpdateRva, std::dec, "; not installing");
    return false;
  }

  nodeOffset.store(game.winPartsNodeOffset, std::memory_order_relaxed);
  translateOffset.store(game.winPartsTranslateOffset, std::memory_order_relaxed);

  if (!installMinHookDetour(target,
                            reinterpret_cast<void*>(&tracedWinPartsUpdate),
                            reinterpret_cast<void**>(&originalWinPartsUpdate))) {
    log("WINPARTS trace: install failed");
    return false;
  }

  tracing.store(true, std::memory_order_relaxed);
  log("WINPARTS trace: active update=0x", std::hex, game.winPartsUpdateRva,
      " node=+0x", game.winPartsNodeOffset,
      " translate=+0x", game.winPartsTranslateOffset, std::dec,
      " (nothing is changed; zero calls is itself the answer to whether this"
      " menu draws through WinParts)");
  return true;
}

}  // namespace atfix
