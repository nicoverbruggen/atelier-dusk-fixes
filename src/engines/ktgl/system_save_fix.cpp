// SPDX-License-Identifier: MIT
//
// See system_save_fix.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "system_save_fix.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/mem.h"
#include "../../core/module_lifetime.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// Both are `void step(this)` on the shared PlatformSteam::{Load,Save} object.
using StepProc = void (STDMETHODCALLTYPE*)(uintptr_t);
// (object, buffer, length, outSize, encode) -> non-zero on success. See the hook
// below for why the fifth argument is the direction and the fourth is ignored by
// the game on the read path.
using CodecProc = uint8_t (STDMETHODCALLTYPE*)(uintptr_t, const void*, uint32_t,
                                               uint32_t*, uint8_t);

StepProc originalLoadStep = nullptr;
StepProc originalSaveStep = nullptr;
CodecProc originalSystemCodec = nullptr;

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

// The values its success exit writes, in one qword store covering both fields:
// `mov qword ptr [rbx+0x10], 6` at escha-en 0x138ba1.
constexpr uint32_t kStateCompleted = 6;
constexpr uint32_t kErrorNone = 0;

// Set when a system-data load completed having read nothing, cleared when one
// genuinely succeeds. While set, system-data saves are refused.
std::atomic<bool> g_systemDataUntrusted{false};

std::atomic<uint32_t> g_loadsRepaired{0};
std::atomic<uint32_t> g_savesRefused{0};

// Which kind the last load handled. The codec is handed only a buffer, so it
// cannot tell system data from game data on its own; the load hook runs first on
// the same load and publishes it. Loads are driven one at a time by the save/load
// state machine, so there is no interleaving to lose here.
std::atomic<bool> g_lastLoadWasSystemData{false};

// English builds only, by decision: the message below is written once, in
// English, and a guessed translation shown to a Japanese or Chinese player would
// be worse than the game's own behaviour. Set at install from the matched row.
bool g_englishBuild = false;

// What the games do instead, and why this exists. Escha & Logy says NOTHING at
// all -- it runs on defaults and the player finds out when their settings are
// gone. Shallie does show a box, but its English build renders a UTF-8 Japanese
// string as Latin-1, so it reads as mojibake with an `err: -3`. Neither tells a
// player what happened or what to do.
// Titled with the game rather than the mod. A player meeting this has a problem
// with their save data, not with the mod, and the box should look like it came
// from the thing they launched. titleName() is narrow and this needs wide, so the
// two names are spelled out here; the pair is exhaustive because this hook only
// installs on KTGL, and Unknown cannot reach it.
const wchar_t* unreadableTitle() {
  switch (currentTitle()) {
    case Title::Shallie: return L"Atelier Shallie DX";
    default:             return L"Atelier Escha & Logy DX";
  }
}
constexpr wchar_t kUnreadableMessage[] =
  L"The system data could not be loaded. The game will continue to work, but "
  L"editing any settings may cause data loss.";

// The game's own box for this failure, which ours replaces on English builds.
// Both games call MessageBoxA -- neither imports the W form at all -- and hand it
// UTF-8 Japanese, so Windows decodes it in the system codepage and the player
// gets mojibake plus an `err: -3`. Ours goes out through MessageBoxW, so this
// detour cannot see it and there is no re-entry to guard against.
//
// Matched narrowly rather than swallowed blindly. The executables carry five of
// these (`save`/`load` captions against user and system, plus a duplicate), and
// only one is the system LOAD failure: caption "load", text beginning "system ".
// The user-load box begins "user " and the save boxes have the other caption, so
// none of them match. It is also armed only while our own dialog is up, so a
// system-load failure we did NOT report still shows the game's box rather than
// disappearing silently.
using MessageBoxAProc = int (WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
MessageBoxAProc originalMessageBoxA = nullptr;
std::atomic<bool> g_suppressGameLoadBox{false};

int WINAPI tracedMessageBoxA(HWND owner, LPCSTR text, LPCSTR caption,
                             UINT type) {
  if (g_suppressGameLoadBox.load(std::memory_order_relaxed) && text && caption &&
      !std::strcmp(caption, "load") && !std::strncmp(text, "system ", 7)) {
    g_suppressGameLoadBox.store(false, std::memory_order_relaxed);
    return IDOK;   // as though the player dismissed it
  }
  return originalMessageBoxA(owner, text, caption, type);
}

DWORD WINAPI unreadableSystemDataThread(LPVOID) {
  MessageBoxW(nullptr, kUnreadableMessage, unreadableTitle(),
              MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
  return 0;
}

// On its own thread, deliberately. The decode runs on the load path, and a modal
// blocks whichever thread shows it; if the game is waiting on that load to finish
// it would wait forever. Handing the box to a thread of its own lets the load
// complete and the game carry on behind it.
void showUnreadableSystemData() {
  // A worker cannot safely run from an image FreeLibrary has unmapped, so the
  // module is pinned before the thread is published. No pin, no thread.
  if (!retainModuleForProcessLifetime())
    return;
  // Armed before ours goes up, because the game's box follows within the same
  // load and the two would otherwise stack.
  if (originalMessageBoxA)
    g_suppressGameLoadBox.store(true, std::memory_order_relaxed);
  if (HANDLE thread = CreateThread(nullptr, 0, &unreadableSystemDataThread,
                                   nullptr, 0, nullptr))
    CloseHandle(thread);
}

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
  g_lastLoadWasSystemData.store(systemData, std::memory_order_relaxed);

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
    //
    // The error field is written with the state because the engine writes both
    // in one store. Setting the state alone leaves a combination the engine
    // never produces -- completed, state 6, and whatever error the constructor
    // set -- so a refused save stays indistinguishable from a real one in every
    // field the engine defines.
    const uint32_t state = kStateCompleted;
    const uint32_t error = kErrorNone;
    const uint8_t done = 1;
    if (readableRange(self + kState, sizeof(state)) &&
        readableRange(self + kError, sizeof(error)) &&
        readableRange(self + kCompleted, sizeof(done))) {
      std::memcpy(reinterpret_cast<void*>(self + kState), &state, sizeof(state));
      std::memcpy(reinterpret_cast<void*>(self + kError), &error, sizeof(error));
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

// The save-data codec, shared by both save kinds and both directions:
//   (object, buffer, length, outSize, encode) -> non-zero on success
//
// The fifth argument is the direction. Every save call site passes 1 and a real
// out-size pointer; every load call site passes 0 and NULL, which is the whole
// asymmetry described in system_save_fix.h -- the write path uses the size it is
// given back and the read path declines to ask for it.
//
// Hooking this is correctly scoped rather than a blanket intercept: the codec
// has exactly four call sites per executable and all four are the save/load
// consumers, so nothing else in the game can reach it.
uint8_t STDMETHODCALLTYPE tracedSystemCodec(uintptr_t self, const void* buffer,
                                            uint32_t length, uint32_t* outSize,
                                            uint8_t encode) {
  const uint8_t ok = originalSystemCodec(self, buffer, length, outSize, encode);
  if (!fixEnabled() || ok || encode)
    return ok;

  // A DECODE failed, which is reported and nothing more. Refusing the write-back
  // here was tried and is wrong, and the reasoning is worth keeping because the
  // opposite looks obviously right:
  //
  // The zero-byte guard above refuses saves because a load that read NOTHING may
  // have a perfectly healthy file behind it -- an open that lost a race with a
  // Cloud sync, a lock, an antivirus scan. Protecting that file is the whole
  // point. A failed decode is the other situation: the bytes were read and they
  // are bad, so there is no good file left to protect. Refusing then preserves
  // an unreadable file forever, and because the game would otherwise replace it,
  // the user gets settings that silently reset on every launch with no way out
  // short of deleting the file by hand.
  //
  // Partial files are the expected source of this, which is what settles it: the
  // write path has no temp file and no rename, so an interrupted save leaves one
  // behind. Letting the game rewrite is the recovery.
  //
  // A transient SHORT READ of a healthy file would also land here and would be
  // worth refusing, but it cannot be told apart from a short file: `ftell` lands
  // at end-of-file either way, and the object carries no expected length.
  static std::atomic<bool> reported{false};
  static std::atomic<bool> announced{false};
  if (!reported.exchange(true, std::memory_order_relaxed))
    log("SYSSAVE save-data decode FAILED on ", std::dec, length,
        " bytes: the file is unreadable, not merely unread. The engine discards"
        " this result and runs on defaults, and it will rewrite the file rather"
        " than be stopped from doing so -- which is the recovery. Reported here"
        " because it is otherwise completely silent.");

  // Tell the player, once, and only about SYSTEM data on an English build. Game
  // data is left alone: Shallie already raises its own load error there, and the
  // engine's dialog is the right place for it rather than a second box from us.
  if (g_englishBuild &&
      g_lastLoadWasSystemData.load(std::memory_order_relaxed) &&
      !announced.exchange(true, std::memory_order_relaxed))
    showUnreadableSystemData();
  return ok;
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
// The codec is byte-identical across all four builds: homolog confirms the pair
// in each game with 104 votes forward and reverse, the same 0x832 size, and this
// same prologue.
constexpr std::array<BYTE, 16> kCodecExpected = {
  0x4c, 0x89, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x54,
  0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55,
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
  if (!game.systemLoadStepRva || !game.systemSaveStepRva ||
      !game.systemCodecRva) {
    log("FIXES system_save=unavailable (no address row for this executable)");
    return false;
  }

  g_englishBuild = game.exeBuild == BuildEnglish;
  const bool shallie = currentTitle() == Title::Shallie;
  const std::array<BYTE, 16>& loadExpected =
    shallie ? kShallieLoadExpected : kEschaLoadExpected;

  BYTE* loadStep = base + game.systemLoadStepRva;
  BYTE* saveStep = base + game.systemSaveStepRva;
  BYTE* codec = base + game.systemCodecRva;
  if (!matches(loadStep, loadExpected) || !matches(saveStep, kSaveExpected) ||
      !matches(codec, kCodecExpected)) {
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
  // Last, because it is report-only: it explains a decode failure to the player
  // and changes nothing else. The latch is set by the load hook and read by the
  // save hook, so those two go first and any partial install leaves the game
  // exactly as it shipped rather than half-guarded.
  const bool codecOk = saveOk && installMinHookDetour(codec,
    reinterpret_cast<void*>(&tracedSystemCodec),
    reinterpret_cast<void**>(&originalSystemCodec));

  const bool ok = loadOk && saveOk && codecOk;
  // English builds only, and never required: this replaces a broken message with
  // a readable one and protects no data, so a failure here must not take the
  // guards down with it.
  if (ok && g_englishBuild) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      if (BYTE* proc = reinterpret_cast<BYTE*>(
            GetProcAddress(user32, "MessageBoxA")))
        if (!installMinHookDetour(proc,
              reinterpret_cast<void*>(&tracedMessageBoxA),
              reinterpret_cast<void**>(&originalMessageBoxA)))
          log("FIXES system_save: the game's own load-error box could not be"
              " replaced; both it and ours will appear");
    }
  }

  log("FIXES system_save=", ok ? "active" : "failed",
      " load_rva=0x", std::hex, game.systemLoadStepRva,
      " save_rva=0x", game.systemSaveStepRva,
      " codec_rva=0x", game.systemCodecRva, std::dec);
  return ok;
}

}  // namespace dusk
