// SPDX-License-Identifier: MIT
//
// See pad_rescan.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>

#include "pad_rescan.h"
#include "game.h"
#include "hook_util.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The wrapper returns the new pad object, or null when no device was found.
using PadCreateProc = void* (STDMETHODCALLTYPE*)(uintptr_t);

PadCreateProc originalPadCreate = nullptr;

// Steady clock rather than the frame counter on purpose: the engine's own gate
// already counts ticks, and counting them again would inherit the very property
// that makes the retry rate depend on refresh rate.
using Clock = std::chrono::steady_clock;
std::atomic<int64_t> g_nextAttemptNanos{0};

// Long enough that the enumeration cost stops mattering, short enough that
// plugging a controller in mid-session is not a noticeable wait.
constexpr int64_t kBackoffNanos = 3'000'000'000;   // 3 s

bool fixEnabled() {
  // Resolved once. tracedPadCreate runs on the engine's controller poll, and
  // hooks on that thread must not touch the ini or the environment; the rule is
  // stated in engines/phyre/logo_skip.cpp. featureEnabled() reaches both.
  static const bool enabled = featureEnabled(Feature::PadRescanBackoff);
  return enabled;
}

void* STDMETHODCALLTYPE tracedPadCreate(uintptr_t self) {
  const int64_t now = Clock::now().time_since_epoch().count();

  // Suppression happens before the call, which is the whole point -- the cost
  // being avoided is inside the original.
  if (fixEnabled()) {
    const int64_t next = g_nextAttemptNanos.load(std::memory_order_relaxed);
    if (next != 0 && now < next)
      return nullptr;
  }

  void* pad = originalPadCreate(self);

  // A pad was found: clear the backoff so the next disconnect is noticed at the
  // engine's own cadence rather than ours. Only a FAILED attempt arms it.
  if (pad)
    g_nextAttemptNanos.store(0, std::memory_order_relaxed);
  else
    g_nextAttemptNanos.store(now + kBackoffNanos, std::memory_order_relaxed);

  return pad;
}

}  // namespace

bool installPadRescanBackoff(BYTE* base, const PadRescanTarget& target) {
  if (!fixEnabled()) {
    log("FIXES pad_rescan=off");
    return false;
  }
  if (!base || !target.wrapperRva) {
    log("FIXES pad_rescan=unavailable (no address row for this executable)");
    return false;
  }

  BYTE* wrapper = base + target.wrapperRva;
  if (!matches(wrapper, target.expected)) {
    log("FIXES pad_rescan=declined (prologue mismatch at rva=0x", std::hex,
        target.wrapperRva, std::dec, ")");
    return false;
  }

  const bool ok = installMinHookDetour(wrapper,
    reinterpret_cast<void*>(&tracedPadCreate),
    reinterpret_cast<void**>(&originalPadCreate));

  log("FIXES pad_rescan=", ok ? "active" : "failed",
      " wrapper_rva=0x", std::hex, target.wrapperRva, std::dec,
      " backoff_s=", kBackoffNanos / 1'000'000'000);
  return ok;
}

}  // namespace atfix
