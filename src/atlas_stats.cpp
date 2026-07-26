// SPDX-License-Identifier: MIT
//
// Ayesha font-atlas diagnostic. Counting only: every hook forwards to the
// original immediately, nothing is cached, nothing is suppressed. The purpose is
// to decide whether the Arland menu-hitch fix is worth porting to Ayesha, and if
// so which cache lifetime it needs -- see TECHNICAL.md 1.8 and 2.
//
// The four hooked entry points and how their addresses were derived are recorded
// in TECHNICAL.md 1.7. Do not change an RVA here without updating that table.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "atlas_stats.h"
#include "game.h"
#include "hook_util.h"
#include "log.h"
#include "util.h"
#include "../vendor/minhook/include/MinHook.h"

namespace atfix {
extern Log log;
}

namespace {

using atfix::Game;
using atfix::BuildEnglish;
using atfix::BuildMultilingual;
using atfix::matches;
using atfix::installMinHookDetour;
using atfix::log;

// Signatures match the Arland project's, which is expected: these are the same
// middleware and text-renderer functions (TECHNICAL.md 1.5).
//
// The atlas lock's 4th argument is the middleware ACCESS MODE, not a cube face:
// 0 maps a staging copy for CPU reading, non-zero maps the texture itself for
// CPU writing (3 being a WRITE_DISCARD of the dynamic resource). The text
// renderer uses both per glyph -- write to rasterize, then read to blit back --
// and that round trip is what the Arland cache collapses. Splitting the counts
// by mode is therefore the whole point of this diagnostic.
using QueueDrainProc = void (*)(void*);
using RenderTextProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using AtlasLockProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using AtlasUnlockProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

QueueDrainProc originalQueueDrain = nullptr;
RenderTextProc originalRenderText = nullptr;
AtlasLockProc originalAtlasLock = nullptr;
AtlasUnlockProc originalAtlasUnlock = nullptr;

// Prologues verified in both Ayesha binaries; see TECHNICAL.md 1.7 for how each
// was derived.
//
// The queue drain, text renderer and lock windows are build-independent. The
// lock's window deliberately ends on the `e8` call opcode and excludes the
// displacement, which in Ayesha targets an incremental-link thunk rather than
// the implementation directly -- that is why it is portable at all.
const std::array<BYTE, 16> kQueueDrainExpected = {
  0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
  0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0x98,
};
const std::array<BYTE, 16> kRenderTextExpected = {
  0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x53,
  0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00, 0x48,
};
const std::array<BYTE, 16> kAtlasLockExpected = {
  0x48, 0x83, 0xec, 0x38, 0x44, 0x89, 0x4c, 0x24,
  0x20, 0x45, 0x8b, 0xc8, 0x45, 0x33, 0xc0, 0xe8,
};

// The hooked unlock is the two-instruction stub at lock+0x40. Its window carries
// the `jmp rel32` displacement to the thunk, so it differs per build and lives
// in the Game row rather than here (Arland precedent: dtorExpectedEn/Multi).
constexpr Game games[] = {
  { "Atelier_Ayesha_EN.exe", 0x984df4,
    0x078320, 0x74bd90, 0x581420, 0x581460,
    { 0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x01, 0xff,
      0xa7, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    BuildEnglish },
  { "Atelier_Ayesha.exe", 0x9a9604,
    0x07a8d0, 0x76e290, 0x5a3920, 0x5a3960,
    { 0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x01, 0xda,
      0xa5, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    BuildMultilingual },
};

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

// Nesting depth of the hooked text renderer on this thread. A lock only counts
// as a font-atlas candidate while the renderer is on the stack, matching the
// Arland cache's eligibility rule.
thread_local unsigned renderTextDepth = 0;
// Nesting depth of the queue drain on this thread, so a re-entrant drain does
// not report itself twice.
thread_local unsigned queueDrainDepth = 0;

struct Bucket {
  uint64_t locks = 0;      // total candidate locks on this texture
  uint64_t readLocks = 0;  // mode 0: staging copy mapped for reading
  uint64_t writeLocks = 0; // mode != 0: texture mapped for CPU writing
};

struct Counters {
  uint64_t candidateLocks = 0;
  uint64_t readLocks = 0;
  uint64_t writeLocks = 0;
  uint64_t nonCandidateLocks = 0;   // lock seen, but not while rendering text
  uint64_t unlocks = 0;
  uint64_t renderTextCalls = 0;
  std::unordered_map<uintptr_t, Bucket> perTexture;
  // Every (width,height) pair seen at a lock while the text renderer is
  // active. This exists to validate the assumption, not just to satisfy it:
  // the +0x40/+0x42 dimension offsets were derived on the Arland middleware,
  // and if they do not hold in Ayesha this histogram shows garbage instead of
  // a clean 512x512 concentration. Read it before trusting any count above.
  std::unordered_map<uint32_t, uint64_t> dimensions;

  void reset() {
    *this = Counters();
  }
};

atfix::mutex countersMutex;
Counters inDrain;      // accumulated within the current queue drain
Counters outOfDrain;   // accumulated since the last Present, outside any drain

// Locks arriving outside any drain are the Rorona-class signal: Rorona issued a
// large batch through the same path *before* entering the drain, which is why
// the Arland fix needed a frame-scoped lifetime there rather than the
// queue-scoped one that sufficed for Totori and Meruru.
std::atomic<uint64_t> drainsObserved{0};

bool statsActive = false;

uint32_t packDims(uint16_t w, uint16_t h) {
  return (uint32_t(w) << 16) | uint32_t(h);
}

void logCounters(const char* scope, const Counters& c, uint64_t micros) {
  // Distinct textures and the worst repeat count are the two numbers that
  // decide whether a cache would pay: the Arland saving came from thousands of
  // reads collapsing onto three atlases.
  uint64_t worstRepeat = 0;
  uintptr_t worstTexture = 0;
  for (const auto& entry : c.perTexture) {
    if (entry.second.locks > worstRepeat) {
      worstRepeat = entry.second.locks;
      worstTexture = entry.first;
    }
  }
  log(scope,
    ": micros=", micros,
    " renderText=", c.renderTextCalls,
    " candidateLocks=", c.candidateLocks,
    " (read=", c.readLocks, " write=", c.writeLocks, ")",
    " nonCandidateLocks=", c.nonCandidateLocks,
    " unlocks=", c.unlocks,
    " distinctTextures=", c.perTexture.size(),
    " worstRepeat=", worstRepeat,
    " worstTexture=", reinterpret_cast<void*>(worstTexture));
  for (const auto& entry : c.dimensions) {
    log("    dims ", (entry.first >> 16), "x", (entry.first & 0xffff),
        " locks=", entry.second);
  }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

uintptr_t statsAtlasLock(uintptr_t texture, uintptr_t output,
                         uintptr_t level, uintptr_t mode) {
  uint16_t width = 0;
  uint16_t height = 0;
  if (texture) {
    const auto* bytes = reinterpret_cast<const BYTE*>(texture);
    std::memcpy(&width, bytes + 0x40, sizeof(width));
    std::memcpy(&height, bytes + 0x42, sizeof(height));
  }

  if (statsActive) {
    std::lock_guard lock(countersMutex);
    Counters& c = queueDrainDepth ? inDrain : outOfDrain;
    if (renderTextDepth && output) {
      ++c.candidateLocks;
      if (mode)
        ++c.writeLocks;
      else
        ++c.readLocks;
      Bucket& bucket = c.perTexture[texture];
      ++bucket.locks;
      if (mode)
        ++bucket.writeLocks;
      else
        ++bucket.readLocks;
      ++c.dimensions[packDims(width, height)];
    } else {
      ++c.nonCandidateLocks;
    }
  }

  return originalAtlasLock(texture, output, level, mode);
}

uintptr_t statsAtlasUnlock(uintptr_t a, uintptr_t b, uintptr_t c,
                           uintptr_t d) {
  if (statsActive) {
    std::lock_guard lock(countersMutex);
    (queueDrainDepth ? inDrain : outOfDrain).unlocks++;
  }
  return originalAtlasUnlock(a, b, c, d);
}

uintptr_t statsRenderText(uintptr_t a, uintptr_t b, uintptr_t c,
                          uintptr_t d) {
  if (statsActive) {
    std::lock_guard lock(countersMutex);
    (queueDrainDepth ? inDrain : outOfDrain).renderTextCalls++;
  }
  ++renderTextDepth;
  const uintptr_t result = originalRenderText(a, b, c, d);
  --renderTextDepth;
  return result;
}

void statsQueueDrain(void* self) {
  if (!statsActive) {
    originalQueueDrain(self);
    return;
  }

  // Only the outermost drain reports, so a re-entrant one is folded into its
  // parent rather than logging a partial line and clearing shared counters.
  const bool outermost = queueDrainDepth == 0;
  if (outermost) {
    std::lock_guard lock(countersMutex);
    inDrain.reset();
  }

  ++queueDrainDepth;
  const auto started = std::chrono::steady_clock::now();
  originalQueueDrain(self);
  const uint64_t micros = uint64_t(
    std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count());
  --queueDrainDepth;

  if (!outermost)
    return;

  const uint64_t index = drainsObserved.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(countersMutex);
  // A drain that touched no text at all is the common idle case; logging every
  // one of those would bury the menu-construction drains that matter.
  if (inDrain.candidateLocks || inDrain.renderTextCalls) {
    log("drain #", index);
    logCounters("  inDrain", inDrain, micros);
  }
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

const Game* recognizeExecutable(BYTE*& baseOut) {
  HMODULE module = GetModuleHandleW(nullptr);
  if (!module)
    return nullptr;
  char path[MAX_PATH] = {};
  if (!GetModuleFileNameA(module, path, sizeof(path)))
    return nullptr;
  const char* back = std::strrchr(path, '\\');
  const char* forward = std::strrchr(path, '/');
  const char* sep = back > forward ? back : forward;
  const char* name = sep ? sep + 1 : path;

  MODULEINFO info = {};
  if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
    return nullptr;

  // .text VirtualSize, the same value the Arland gate verifies. Read from the
  // loaded headers rather than the file so a packed-on-disk build would still
  // match after its stub decrypted the section (not currently a factor for
  // Ayesha, but the Arland project hit it with Meruru's SteamStub wrapper).
  auto* base = reinterpret_cast<BYTE*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return nullptr;
  DWORD textSize = 0;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if (!std::memcmp(section->Name, ".text", 5)) {
      textSize = section->Misc.VirtualSize;
      break;
    }
  }

  for (const Game& game : games) {
    if (_stricmp(name, game.executable) || textSize != game.textSize)
      continue;
    baseOut = base;
    return &game;
  }
  log("atlas stats: unrecognized executable ", name,
      " .text=", reinterpret_cast<void*>(uintptr_t(textSize)));
  return nullptr;
}

bool installHooks() {
  BYTE* base = nullptr;
  const Game* game = recognizeExecutable(base);
  if (!game)
    return false;

  auto* queue = base + game->queueDrainRva;
  auto* render = base + game->renderTextRva;
  auto* lock = base + game->atlasLockRva;
  auto* unlock = base + game->atlasUnlockRva;

  if (!matches(queue, kQueueDrainExpected) ||
      !matches(render, kRenderTextExpected) ||
      !matches(lock, kAtlasLockExpected) ||
      !matches(unlock, game->unlockExpected)) {
    log("atlas stats: prologue verification failed, installing nothing");
    return false;
  }

  // Same ordering rule as the Arland installs: the hooks that only observe
  // state go in first, and the drain -- which is what arms reporting -- goes in
  // last, so any partial install is inert rather than half-counting.
  if (!installMinHookDetour(unlock, reinterpret_cast<void*>(&statsAtlasUnlock),
                            reinterpret_cast<void**>(&originalAtlasUnlock)))
    return false;
  if (!installMinHookDetour(lock, reinterpret_cast<void*>(&statsAtlasLock),
                            reinterpret_cast<void**>(&originalAtlasLock)))
    return false;
  if (!installMinHookDetour(render, reinterpret_cast<void*>(&statsRenderText),
                            reinterpret_cast<void**>(&originalRenderText)))
    return false;
  if (!installMinHookDetour(queue, reinterpret_cast<void*>(&statsQueueDrain),
                            reinterpret_cast<void**>(&originalQueueDrain)))
    return false;

  log("atlas stats: installed on ", game->executable,
      " base=", reinterpret_cast<void*>(base),
      " queueDrain=", reinterpret_cast<void*>(uintptr_t(game->queueDrainRva)),
      " renderText=", reinterpret_cast<void*>(uintptr_t(game->renderTextRva)),
      " atlasLock=", reinterpret_cast<void*>(uintptr_t(game->atlasLockRva)),
      " atlasUnlock=", reinterpret_cast<void*>(uintptr_t(game->atlasUnlockRva)));
  return true;
}

}  // namespace

namespace dusk {

bool initializeAtlasStats() {
  static bool installed = [] {
    if (!atfix::featureEnabled(atfix::Feature::AtlasStats))
      return false;
    if (MH_Initialize() != MH_OK) {
      log("atlas stats: MH_Initialize failed");
      return false;
    }
    const bool ok = installHooks();
    statsActive = ok;
    return ok;
  }();
  return installed;
}

void atlasStatsFrameTick() {
  if (!statsActive)
    return;
  std::lock_guard lock(countersMutex);
  // Locks that happened this frame but outside any drain. Rorona's pre-drain
  // batch showed up exactly here, and it is what forced the frame-scoped cache
  // lifetime, so a nonzero count on Ayesha changes which port is correct.
  if (outOfDrain.candidateLocks || outOfDrain.renderTextCalls)
    logCounters("outOfDrain (frame)", outOfDrain, 0);
  outOfDrain.reset();
}

}  // namespace dusk
