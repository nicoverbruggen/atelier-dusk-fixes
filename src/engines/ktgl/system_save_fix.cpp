// SPDX-License-Identifier: MIT
//
// See system_save_fix.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "system_save_fix.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/mem.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// Both are `void step(this)` on the shared PlatformSteam::{Load,Save} object.
using StepProc = void (STDMETHODCALLTYPE*)(uintptr_t);

StepProc originalLoadStep = nullptr;
StepProc originalSaveStep = nullptr;

// PlatformSteam::{Load,Save} share one 0x438-byte object. Both KTGL games and
// both language builds access these same offsets in their load/save steps.
constexpr uintptr_t kState = 0x10;         // dword; 6 = completed, 7 = read failed
constexpr uintptr_t kError = 0x14;         // dword; 5 accompanies state 7
constexpr uintptr_t kIsSystemData = 0x418; // byte; 0 selects GAMEDATA
constexpr uintptr_t kBytesRead = 0x428;    // qword, recovered with ftell
constexpr uintptr_t kCompleted = 0x430;    // byte; the flag the caller gates on

// The values the engine's own read-failure branch writes, four instructions
// above the shared exit. Reusing them is what keeps this correction inside
// behaviour the game already produces.
constexpr uint32_t kStateReadFailed = 7;
constexpr uint32_t kErrorReadFailed = 5;

// Set when a system-data load completed having read nothing, cleared when one
// genuinely succeeds. While set, system-data saves are refused.
std::atomic<bool> g_systemDataUntrusted{false};

std::atomic<uint32_t> g_loadsRepaired{0};
std::atomic<uint32_t> g_savesRefused{0};

bool fixEnabled() {
  return featureEnabled(Feature::SystemSaveGuard);
}

// Load and save share this object. A false result means the discriminator could
// not be read and the hook must decline; `systemData == false` is the valid
// GAMEDATA kind, not a read failure.
bool readSystemDataKind(uintptr_t self, bool& systemData) {
  uint8_t flag = 0;
  if (!tryRead(self + kIsSystemData, flag))
    return false;
  systemData = flag != 0;
  return true;
}

void STDMETHODCALLTYPE tracedLoadStep(uintptr_t self) {
  originalLoadStep(self);
  if (!fixEnabled() || !self)
    return;

  bool systemData = false;
  if (!readSystemDataKind(self, systemData))
    return;

  uint8_t completed = 0;
  uint64_t bytesRead = 0;
  if (!tryRead(self + kCompleted, completed) ||
      !tryRead(self + kBytesRead, bytesRead))
    return;
  if (!completed)
    return;   // the engine already knows this one failed

  if (bytesRead != 0) {
    // A real load. Release the latch so a session that recovers -- a Steam Cloud
    // sync completing, a lock being dropped -- can save normally again.
    if (systemData &&
        g_systemDataUntrusted.exchange(false, std::memory_order_relaxed))
      log("SYSSAVE system data loaded (", std::dec, bytesRead,
          " bytes); saves re-enabled");
    return;
  }

  // Completed and nothing was read. That is the defect: an open failure or an
  // empty file arriving at the success exit. Put the object into the state the
  // engine's own read-failure branch would have set.
  const uint32_t state = kStateReadFailed;
  const uint32_t error = kErrorReadFailed;
  const uint8_t clear = 0;
  if (!readableRange(self + kState, sizeof(state)) ||
      !readableRange(self + kError, sizeof(error)) ||
      !readableRange(self + kCompleted, sizeof(clear)))
    return;
  std::memcpy(reinterpret_cast<void*>(self + kState), &state, sizeof(state));
  std::memcpy(reinterpret_cast<void*>(self + kError), &error, sizeof(error));
  std::memcpy(reinterpret_cast<void*>(self + kCompleted), &clear, sizeof(clear));

  if (systemData)
    g_systemDataUntrusted.store(true, std::memory_order_relaxed);
  const uint32_t n = g_loadsRepaired.fetch_add(1, std::memory_order_relaxed) + 1;
  if (systemData) {
    log("SYSSAVE system-data load reported success having read 0 bytes --"
        " forced to the engine's read-failure state, and system-data saves"
        " are now refused (n=", std::dec, n, ")");
  } else {
    log("SYSSAVE GAMEDATA load reported success having read 0 bytes -- forced"
        " to the engine's read-failure state; live game data was not replaced"
        " (n=", std::dec, n, ")");
  }
}

void STDMETHODCALLTYPE tracedSaveStep(uintptr_t self) {
  bool systemData = false;
  if (fixEnabled() && self && readSystemDataKind(self, systemData) &&
      systemData && g_systemDataUntrusted.load(std::memory_order_relaxed)) {
    // Refusing means not calling through at all: the truncation happens inside,
    // at fopen(L"wb"), so anything that reaches the original has already
    // destroyed the file whatever it does afterwards.
    //
    // The object is left reporting completion so the caller's poll terminates.
    // It asked for a write and is told the write finished; what it is not told
    // is that the bytes it wanted written were defaults.
    const uint32_t state = 6;
    const uint8_t done = 1;
    if (readableRange(self + kState, sizeof(state)) &&
        readableRange(self + kCompleted, sizeof(done))) {
      std::memcpy(reinterpret_cast<void*>(self + kState), &state, sizeof(state));
      std::memcpy(reinterpret_cast<void*>(self + kCompleted), &done, sizeof(done));
    }
    const uint32_t n = g_savesRefused.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1)
      log("SYSSAVE refusing system-data save: the last system load produced"
          " nothing, so writing now would replace good data with defaults."
          " GAMEDATA saves are unaffected.");
    return;
  }
  originalSaveStep(self);
}

// Per-game prologue windows. Both are byte-identical between the English and
// multilingual build of the same game -- they are the same compile of the same
// source -- so there is one pair per game rather than one per executable.
constexpr std::array<BYTE, 16> kEschaLoadExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83,
  0xec, 0x30, 0x48, 0x8b, 0xd9, 0x48, 0xc7, 0x44,
};
constexpr std::array<BYTE, 16> kShallieLoadExpected = {
  0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8d,
  0x41, 0x18, 0x48, 0xc7, 0x44, 0x24, 0x40, 0x00,
};
// Save::step is byte-identical across all four builds.
constexpr std::array<BYTE, 16> kSaveExpected = {
  0x40, 0x53, 0x57, 0x48, 0x81, 0xec, 0x38, 0x01,
  0x00, 0x00, 0x48, 0x8d, 0x79, 0x18, 0x48, 0x8b,
};

}  // namespace
}  // namespace atfix

namespace dusk {

bool installSystemSaveFix(BYTE* base, const atfix::KtglGame& game) {
  using namespace atfix;
  auto& log = atfix::log;

  if (!fixEnabled()) {
    log("FIXES system_save=off");
    return false;
  }
  if (!game.systemLoadStepRva || !game.systemSaveStepRva) {
    log("FIXES system_save=unavailable (no address row for this executable)");
    return false;
  }

  const bool shallie = currentTitle() == Title::Shallie;
  const std::array<BYTE, 16>& loadExpected =
    shallie ? kShallieLoadExpected : kEschaLoadExpected;

  BYTE* loadStep = base + game.systemLoadStepRva;
  BYTE* saveStep = base + game.systemSaveStepRva;
  if (!matches(loadStep, loadExpected) || !matches(saveStep, kSaveExpected)) {
    log("FIXES system_save=declined (prologue mismatch)");
    return false;
  }

  // The load hook goes in first. Without it the latch is never set, so a
  // half-installed pair refuses nothing and leaves the game exactly as it
  // shipped -- rather than refusing saves on a latch nothing can clear.
  const bool loadOk = installMinHookDetour(loadStep,
    reinterpret_cast<void*>(&tracedLoadStep),
    reinterpret_cast<void**>(&originalLoadStep));
  const bool saveOk = loadOk && installMinHookDetour(saveStep,
    reinterpret_cast<void*>(&tracedSaveStep),
    reinterpret_cast<void**>(&originalSaveStep));

  log("FIXES system_save=", loadOk && saveOk ? "active" : "failed",
      " load_rva=0x", std::hex, game.systemLoadStepRva,
      " save_rva=0x", game.systemSaveStepRva, std::dec);
  return loadOk && saveOk;
}

}  // namespace dusk
