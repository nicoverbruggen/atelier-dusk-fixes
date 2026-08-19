// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// present_throttle.h; what is here is the mechanism and the notes that only
// mean anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "present_throttle.h"
#include "config.h"
#include "game.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// PhyreEngine's own answer, 0x21 in all four builds. Read the header for where
// each one lives.
constexpr unsigned long kEngineThrottleMs = 33;
// A value above this is a typo rather than a request. A second of sleep per
// frame would make a restore feel broken.
constexpr unsigned long kMaxThrottleMs = 1000;

unsigned long throttleMs() {
  static const unsigned long milliseconds = [] () -> unsigned long {
    if (!featureEnabled(Feature::MinimizedThrottle))
      return 0;
    const char* value = std::getenv("DUSK_MINIMIZED_THROTTLE");
    if (!value)
      return kEngineThrottleMs;
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed < 0 || parsed > long(kMaxThrottleMs))
      return kEngineThrottleMs;
    return static_cast<unsigned long>(parsed);
  }();
  return milliseconds;
}

// The output window, resolved once. It is fixed for the swap chain's life, and
// asking the swap chain for its description every frame to learn something that
// cannot change is the kind of per-frame cost this module exists to remove.
HWND outputWindow(IDXGISwapChain* swapChain) {
  static std::atomic<HWND> cached{ nullptr };
  static std::atomic<bool> resolved{ false };
  if (resolved.load(std::memory_order_acquire))
    return cached.load(std::memory_order_acquire);
  HWND window = nullptr;
  DXGI_SWAP_CHAIN_DESC desc = { };
  if (swapChain && SUCCEEDED(swapChain->GetDesc(&desc)))
    window = desc.OutputWindow;
  cached.store(window, std::memory_order_release);
  resolved.store(true, std::memory_order_release);
  return window;
}

// The diagnostic that established the defect, kept because it is the only way
// to tell whether a later run is still occluded and at what rate. One line on
// each change of state, and twice a second while hidden; a visible window
// reports once and then stays quiet, so an ordinary session costs one line.
void traceOcclusion(bool iconic, bool occluded, long result) {
  if (!verboseLogging())
    return;
  static bool lastIconic = false;
  static bool lastOccluded = false;
  static uint64_t markMs = 0;
  static uint64_t markFrames = 0;
  static uint64_t frames = 0;
  ++frames;
  const uint64_t now = GetTickCount64();
  if (!markMs) {
    markMs = now;
    markFrames = frames;
  }
  const bool changed = iconic != lastIconic || occluded != lastOccluded;
  const uint64_t elapsed = now - markMs;
  if (!changed && !(iconic && elapsed >= 500))
    return;
  lastIconic = iconic;
  lastOccluded = occluded;
  const uint64_t counted = frames - markFrames;
  markMs = now;
  markFrames = frames;
  log("PRESENT_OCCLUSION iconic=", iconic ? 1 : 0,
      " occluded=", occluded ? 1 : 0,
      " hr=0x", std::hex, uint32_t(result), std::dec,
      " frames=", counted, " over_ms=", elapsed,
      " fps=", elapsed ? (counted * 1000) / elapsed : 0);
}

}  // namespace

void presentThrottleAfterPresent(IDXGISwapChain* swapChain, long result) {
  const unsigned long milliseconds = throttleMs();
  // Nothing is asked of a visible frame beyond this compare when the row is
  // Unsupported or the switch is 0.
  if (!milliseconds && !verboseLogging())
    return;

  const bool occluded = result == DXGI_STATUS_OCCLUDED;
  // Only ask the window when the HRESULT did not already answer. Every
  // measurement had the two agreeing, so this is the second opinion for a
  // runtime whose occlusion reporting has not been measured, not the primary.
  const HWND window = outputWindow(swapChain);
  const bool iconic = window && IsIconic(window);

  traceOcclusion(iconic, occluded, result);

  // AFTER Present, deliberately. The frame is already gone, so the sleep delays
  // only the next one, and nothing that was going to be drawn is held back.
  if (milliseconds && (occluded || iconic))
    Sleep(milliseconds);
}

}  // namespace atfix
