// SPDX-License-Identifier: MIT
//
// See worker_idle_sleep.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "worker_idle_sleep.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/util.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using SleepProc = void (WINAPI*)(DWORD);
SleepProc originalSleep = nullptr;

// The value the game passes, and the length of `mov ecx, imm32` plus the `call`
// that follows it, so the return address is the site plus ten.
constexpr DWORD kGameIdleSleep = 500;
constexpr uintptr_t kCallLength = 0xa;
constexpr uint32_t kDefaultMs = 10;

// mov ecx, 0x1f4 / call. Identical in every PhyreEngine build; only the two
// displacements differ, so the six bytes are the whole gate.
constexpr std::array<BYTE, 6> kExpected = {
  0xb9, 0xf4, 0x01, 0x00, 0x00, 0xe8,
};

// Resolved at install, so the hook compares one number per call rather than
// consulting a table.
std::atomic<uintptr_t> g_siteReturn = { 0 };
std::atomic<uint32_t> g_overrideMs = { 0 };

uint32_t requestedMs() {
  static const uint32_t requested = [] () -> uint32_t {
    const char* value = std::getenv("DUSK_WORKER_IDLE_SLEEP");
    if (!value)
      return kDefaultMs;
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 && parsed <= 500 ? uint32_t(parsed) : 0;
  }();
  return requested;
}

void WINAPI tracedSleep(DWORD milliseconds) {
  // Two cheap tests before the return address is even read: this detour sits on
  // a system call every thread in the process makes.
  if (milliseconds == kGameIdleSleep) {
    const uintptr_t site = g_siteReturn.load(std::memory_order_relaxed);
    if (site && reinterpret_cast<uintptr_t>(duskReturnAddress()) == site)
      milliseconds = g_overrideMs.load(std::memory_order_relaxed);
  }
  originalSleep(milliseconds);
}

}  // namespace

bool installWorkerIdleSleep(BYTE* base, uintptr_t sleepSiteRva) {
  if (!featureEnabled(Feature::WorkerIdleSleep)) {
    log("FIXES worker_idle_sleep=off");
    return false;
  }
  const uint32_t requested = requestedMs();
  if (!requested) {
    log("FIXES worker_idle_sleep=off");
    return false;
  }
  if (!base || !sleepSiteRva) {
    log("FIXES worker_idle_sleep=unavailable (no address row for this build)");
    return false;
  }
  BYTE* site = base + sleepSiteRva;
  if (!matches(site, kExpected)) {
    log("FIXES worker_idle_sleep=declined (no Sleep(500) at rva=0x",
      std::hex, sleepSiteRva, std::dec, "; this build differs)");
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  auto* sleepProc = kernel32
    ? reinterpret_cast<BYTE*>(GetProcAddress(kernel32, "Sleep")) : nullptr;
  if (!sleepProc) {
    log("FIXES worker_idle_sleep=unavailable (no kernel32!Sleep)");
    return false;
  }

  // Published before the detour goes in, so the first call through it already
  // sees a complete pair.
  g_overrideMs.store(requested, std::memory_order_relaxed);
  g_siteReturn.store(reinterpret_cast<uintptr_t>(site) + kCallLength,
    std::memory_order_relaxed);

  const bool ok = installMinHookDetour(sleepProc,
    reinterpret_cast<void*>(&tracedSleep),
    reinterpret_cast<void**>(&originalSleep));
  if (!ok) {
    g_siteReturn.store(0, std::memory_order_relaxed);
    log("FIXES worker_idle_sleep=failed (Sleep detour did not install)");
    return false;
  }
  log("FIXES worker_idle_sleep=active ms=", requested,
    " (game default 500) rva=0x", std::hex, sleepSiteRva, std::dec);
  return true;
}

}  // namespace atfix
