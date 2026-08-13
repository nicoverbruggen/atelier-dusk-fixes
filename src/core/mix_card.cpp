// SPDX-License-Identifier: MIT
//
// See mix_card.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "mix_card.h"
#include "game.h"
#include "hook_util.h"
#include "log.h"
#include "mem.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// bool __fastcall Card::Update(Card* self, float dt). The return is `mov al, 1`
// on the single exit -- it is always true.
using CardUpdateProc = bool (STDMETHODCALLTYPE*)(uintptr_t, float);

CardUpdateProc originalCardUpdate = nullptr;

// The accumulator the pump reads, adds dt to, and writes back.
constexpr uintptr_t kAccumulator = 0x820;

// The engine's own constants, as exact bit patterns rather than decimal
// literals: the predicate is a truncation, so a compiler that rounded these
// differently would decide a tick was due on a different frame than the
// original does. Verified byte-for-byte in every build.
//   0x3C88AB86 = 0.016683351f  (1/59.94, the authored step)
//   0x426FC28F = 59.939999f    (the authored rate)
float bitsToFloat(uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

const float kStep = bitsToFloat(0x3C88AB86u);
const float kRate = bitsToFloat(0x426FC28Fu);

// A guard against inheriting drift rather than a behaviour change. Running
// unfixed above 60 Hz walks the accumulator unboundedly negative, so a session
// that enables this mid-run could otherwise sit below the tick threshold for a
// long time and appear frozen. Normal operation keeps the value within one step
// of zero; anything a whole second in the past is not recoverable state.
constexpr float kDriftFloor = -1.0f;

bool fixEnabled() {
  return featureEnabled(Feature::SynthesisAnimationRate);
}

bool STDMETHODCALLTYPE tracedCardUpdate(uintptr_t self, float dt) {
  if (!fixEnabled() || !self)
    return originalCardUpdate(self, dt);

  float accumulated = 0.0f;
  if (!tryRead(self + kAccumulator, accumulated))
    return originalCardUpdate(self, dt);   // unreadable: leave the engine alone

  if (accumulated < kDriftFloor)
    accumulated = 0.0f;

  const float next = accumulated + dt;

  // The engine's own predicate, evaluated the way the engine evaluates it. When
  // a tick is due the original runs completely untouched -- it re-reads the same
  // field, recomputes the same count, and behaves bit-for-bit as shipped.
  if (static_cast<int>(next * kRate) >= 1)
    return originalCardUpdate(self, dt);

  // No tick this frame. Bank the elapsed time so it is not lost; the next frame
  // that crosses the threshold spends it.
  if (readableRange(self + kAccumulator, sizeof(next))) {
    std::memcpy(reinterpret_cast<void*>(self + kAccumulator), &next,
                sizeof(next));
  }
  return true;   // the original's only exit is `mov al, 1`
}

}  // namespace

bool installMixCardFix(BYTE* base, const MixCardTarget& target) {
  if (!fixEnabled()) {
    log("FIXES synthesis_rate=off");
    return false;
  }
  if (!base || !target.updateRva) {
    log("FIXES synthesis_rate=unavailable (no address row for this executable)");
    return false;
  }

  BYTE* update = base + target.updateRva;
  if (!matches(update, target.expected)) {
    log("FIXES synthesis_rate=declined (prologue mismatch at rva=0x", std::hex,
        target.updateRva, std::dec, ")");
    return false;
  }

  const bool ok = installMinHookDetour(update,
    reinterpret_cast<void*>(&tracedCardUpdate),
    reinterpret_cast<void**>(&originalCardUpdate));

  log("FIXES synthesis_rate=", ok ? "active" : "failed",
      " update_rva=0x", std::hex, target.updateRva, std::dec);
  return ok;
}

}  // namespace atfix
