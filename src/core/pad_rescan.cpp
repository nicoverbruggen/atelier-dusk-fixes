// SPDX-License-Identifier: MIT
//
// See pad_rescan.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>

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

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_suppressed{0};
std::atomic<uint64_t> g_ranMicros{0};
std::atomic<uint64_t> g_worstMicros{0};
std::atomic<int64_t> g_lastReportNanos{0};

// Long enough that the enumeration cost stops mattering, short enough that
// plugging a controller in mid-session is not a noticeable wait.
constexpr int64_t kBackoffNanos = 3'000'000'000;   // 3 s

// Reporting cadence for the probe. The question it answers -- how long does the
// real call take -- needs a handful of samples, not a line per attempt.
constexpr int64_t kReportNanos = 10'000'000'000;   // 10 s

bool fixEnabled() {
  return featureEnabled(Feature::PadRescanBackoff);
}

bool probeEnabled() {
  const char* value = std::getenv("DUSK_PAD_RESCAN_PROBE");
  return value && value[0] != '0';
}

void* STDMETHODCALLTYPE tracedPadCreate(uintptr_t self) {
  const bool probe = probeEnabled();
  const int64_t now = Clock::now().time_since_epoch().count();

  // Suppression happens before the call, which is the whole point -- the cost
  // being avoided is inside the original.
  if (fixEnabled()) {
    const int64_t next = g_nextAttemptNanos.load(std::memory_order_relaxed);
    if (next != 0 && now < next) {
      g_suppressed.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
  }

  g_calls.fetch_add(1, std::memory_order_relaxed);
  const Clock::time_point started = Clock::now();
  void* pad = originalPadCreate(self);
  const int64_t elapsedMicros =
    std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - started).count();

  g_ranMicros.fetch_add(static_cast<uint64_t>(elapsedMicros),
                        std::memory_order_relaxed);
  uint64_t worst = g_worstMicros.load(std::memory_order_relaxed);
  while (static_cast<uint64_t>(elapsedMicros) > worst &&
         !g_worstMicros.compare_exchange_weak(worst,
           static_cast<uint64_t>(elapsedMicros), std::memory_order_relaxed)) {
  }

  // A pad was found: clear the backoff so the next disconnect is noticed at the
  // engine's own cadence rather than ours. Only a FAILED attempt arms it.
  if (pad)
    g_nextAttemptNanos.store(0, std::memory_order_relaxed);
  else
    g_nextAttemptNanos.store(now + kBackoffNanos, std::memory_order_relaxed);

  if (probe) {
    const int64_t lastReport = g_lastReportNanos.load(std::memory_order_relaxed);
    if (lastReport == 0) {
      g_lastReportNanos.store(now, std::memory_order_relaxed);
    } else if (now - lastReport >= kReportNanos) {
      g_lastReportNanos.store(now, std::memory_order_relaxed);
      const uint64_t calls = g_calls.load(std::memory_order_relaxed);
      const uint64_t micros = g_ranMicros.load(std::memory_order_relaxed);
      // worst_us is the number that decides this feature. An average hides a
      // periodic spike, which is exactly the shape being looked for.
      log("PADRESCAN calls=", std::dec, calls,
          " suppressed=", g_suppressed.load(std::memory_order_relaxed),
          " total_us=", micros,
          " mean_us=", calls ? micros / calls : 0,
          " worst_us=", g_worstMicros.load(std::memory_order_relaxed),
          " fix=", fixEnabled() ? 1 : 0);
    }
  }
  return pad;
}

}  // namespace

bool installPadRescanBackoff(BYTE* base, const PadRescanTarget& target) {
  const bool diagnostic = probeEnabled();
  if (!fixEnabled() && !diagnostic) {
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
      " backoff_s=", kBackoffNanos / 1'000'000'000,
      " probe=", diagnostic ? 1 : 0);
  return ok;
}

}  // namespace atfix
