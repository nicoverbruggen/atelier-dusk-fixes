// SPDX-License-Identifier: MIT
//
// Ayesha font-atlas read cache and its opt-in correctness verifier.
//
// Both ride the same three hooked entry points. Each RVA and prologue window is
// verified independently for both executable builds; do not change one without
// re-deriving its row.
//
// DUSK_ATLAS_CACHE serves repeated 512x512 font-atlas locks from a CPU snapshot.
// Two cheap session totals remain in the normal log so a report can establish
// that the cache is doing useful work. DUSK_ATLAS_VERIFY is the deliberately
// slow, opt-in correctness check for snapshots the cache is about to serve.
//
// Lifetime is FRAME-SCOPED, i.e. the Arland Rorona path rather than the
// Totori/Meruru one. That is a measured choice, not a default: 72% of Ayesha's
// candidate locks arrive outside the resource-event queue drain, so a
// queue-scoped cache would miss most of the work. Snapshots are therefore
// discarded at Present, and never held across a frame boundary.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "atlas_fix.h"
#include "phyre.h"
#include "../../core/config.h"        // verboseLogging
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/util.h"

namespace atfix {
extern Log log;
}

namespace {

using atfix::PhyreGame;
using atfix::matches;
using atfix::installMinHookDetour;
using atfix::log;

// Signatures match the Arland project's, which is expected: these are the same
// middleware and text-renderer functions.
//
// The atlas lock's 4th argument is the middleware ACCESS MODE, not a cube face:
// 0 maps a staging copy for CPU reading, non-zero maps the texture itself for
// CPU writing (3 being a WRITE_DISCARD of the dynamic resource). The text
// renderer uses both per glyph -- write to rasterize, then read to blit back --
// and that round trip is what the Arland cache collapses.
using RenderTextProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using AtlasLockProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using AtlasUnlockProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

RenderTextProc originalRenderText = nullptr;
AtlasLockProc originalAtlasLock = nullptr;
AtlasUnlockProc originalAtlasUnlock = nullptr;

// Prologues verified in both Ayesha binaries; each was derived separately.
//
// The text renderer and lock windows are build-independent. The lock's window
// deliberately ends on the `e8` call opcode and excludes the displacement,
// which in Ayesha targets an incremental-link thunk rather than the
// implementation directly. That is why it is portable at all.
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
// in the PhyreGame row in phyre.cpp rather than here (Arland precedent:
// dtorExpectedEn/Multi).

// Nesting depth of the hooked text renderer on this thread. A lock only counts
// as a font-atlas candidate while the renderer is on the stack, matching the
// Arland cache's eligibility rule.
thread_local unsigned renderTextDepth = 0;
bool cacheActive = false;
bool verifyActive = false;
uint64_t frameIndex = 0;
// Lightweight session health counters. Unlike the removed timing/census
// diagnostics, these add one relaxed increment per eligible lock and let an
// ordinary bug report prove that the cache is active and paying for itself.
std::atomic<uint64_t> atlasCacheHits{0};
std::atomic<uint64_t> atlasRealLocks{0};

// ---------------------------------------------------------------------------
// The cache
// ---------------------------------------------------------------------------

struct AtlasRead {
  uint32_t pitch = 0;
  std::vector<uint8_t> bytes;
  // True once a WRITE-mode lock has been served from this snapshot. The game
  // then rasterizes into our buffer instead of the real texture, so the two
  // legitimately diverge and the verifier must stop comparing them. Cleared only
  // by taking a fresh snapshot, which happens at the next real read.
  bool absorbedWrite = false;
};

atfix::mutex atlasMutex;
std::unordered_map<uintptr_t, std::shared_ptr<AtlasRead>> atlasReads;

// True while snapshots may be created and served. Frame-scoped: raised at
// Present and never lowered by the drain, so locks outside the drain -- 72% of
// them in Ayesha -- are covered too.
std::atomic<bool> atlasCacheArmed{false};

// A lock we satisfied from a snapshot without calling the middleware. Its
// matching unlock must be suppressed, since no real mapping exists to release.
// Per-thread, and each entry keeps its snapshot alive by shared_ptr so a
// concurrent clear or invalidation on another thread cannot free the buffer the
// caller is still reading.
struct SyntheticAtlasLock {
  uintptr_t texture = 0;
  std::shared_ptr<AtlasRead> snapshot;
};
thread_local std::vector<SyntheticAtlasLock> syntheticAtlasLocks;
// Real candidate locks we let through, so their unlocks are recognized as ours
// and do not invalidate the snapshot.
thread_local std::vector<uintptr_t> realCandidateAtlasLocks;

// ---------------------------------------------------------------------------
// Snapshot verification (DUSK_ATLAS_VERIFY)
// ---------------------------------------------------------------------------
//
// Answers the question a playthrough cannot: is a snapshot the cache is about to
// serve actually still what the real atlas contains?
//
// The failure this exists for is the Arland missing-kanji bug: a glyph paged into
// the atlas by a path the mod did not intercept, after the snapshot was taken,
// so the read that follows is served stale and blits blank. Static analysis
// establishes that every atlas unmap in the image routes through the hooked
// unlock, so the invalidation rule *should* catch such a write -- but "should"
// is not evidence, and the case has never been observed either happening or
// being caught.
//
// The invariant checked is exact and has no false positives:
//
//     while a snapshot has NOT absorbed one of our own served writes,
//     it must be byte-identical to the real texture.
//
// Once the cache serves a WRITE lock from a snapshot, the game rasterizes into
// our buffer rather than the real texture, so divergence from that point is
// expected and the check stops for that snapshot until the frame boundary
// replaces it. Every divergence reported before that point is a write the cache
// did not know about -- exactly the dangerous class.
//
// The cost is a real read lock plus a ~1 MB memcmp per verified read, which is
// why this is a diagnostic and never ships on. It calls the ORIGINALS directly,
// so it does not re-enter the mod's own hooks and is invisible to the counters.

std::atomic<uint64_t> verifyChecks{0};
std::atomic<uint64_t> verifyMismatches{0};
std::atomic<uint64_t> verifyForeignWrites{0};
// How many real write locks were examined for the foreign-write case. Without a
// denominator "foreignWrites=0" is unfalsifiable -- it reads the same whether
// the hazard never occurred or the test never ran.
std::atomic<uint64_t> verifyWritesExamined{0};
constexpr uint64_t kVerifyDetailLimit = 16;
// Verify mode makes the game slow, so these are in the tens of seconds, not the
// tens of minutes. The early one exists so a run confirms within a few seconds
// that the check is actually running.
constexpr uint64_t kVerifyFirstReportFrame = 60;
constexpr uint64_t kVerifyReportInterval = 300;

// The comparison above runs once per snapshot -- at the first hit after it is
// created, before the game's own next write marks it diverged. That leaves a
// window: a foreign write arriving later in the same frame.
//
// This closes it, exactly and for free. A WRITE-mode lock only reaches the real
// middleware when the cache did NOT serve it, and the cache serves every
// cacheable write for which a snapshot exists. So a real write lock on a 512x512
// atlas that already has a live snapshot is, necessarily, a write from outside
// the text renderer -- the foreign write the missing-kanji bug is made of.
//
// The invalidation rule is supposed to catch it at the matching unlock. But the
// write mapping is object-held rather than scoped (it is opened and closed by
// different functions), so an unknown amount of time can pass between the write
// and its unlock, and any read served in that window gets stale bytes. Detecting
// the write itself does not depend on when the unlock lands.
void noteForeignWriteOverLiveSnapshot(uintptr_t texture) {
  const uint64_t n = verifyForeignWrites.fetch_add(1, std::memory_order_relaxed);
  if (n < kVerifyDetailLimit)
    log("ATLASVERIFY FOREIGN WRITE over a live snapshot tex=",
        reinterpret_cast<void*>(texture),
        " renderTextDepth=", renderTextDepth,
        " -- a read served before its unlock would have been stale");
  else if (n == kVerifyDetailLimit)
    log("ATLASVERIFY further foreign writes suppressed; the count continues");
}

// Caller must NOT hold atlasMutex, and must hold a shared_ptr to `snap` so it
// cannot be freed underneath the comparison.
void verifySnapshot(uintptr_t texture, const AtlasRead& snap, uint16_t height) {
  void* mapped = nullptr;
  const uintptr_t pitch = originalAtlasLock(
    texture, reinterpret_cast<uintptr_t>(&mapped), 0, 0);
  if (!pitch || !mapped) {
    // Nothing mapped means nothing to compare against; not a finding.
    if (pitch)
      originalAtlasUnlock(texture, 0, 0, 0);
    return;
  }

  verifyChecks.fetch_add(1, std::memory_order_relaxed);
  const size_t size = size_t(pitch) * height;
  const auto* real = static_cast<const uint8_t*>(mapped);

  if (pitch != snap.pitch || size != snap.bytes.size()) {
    const uint64_t n = verifyMismatches.fetch_add(1, std::memory_order_relaxed);
    if (n < kVerifyDetailLimit)
      log("ATLASVERIFY MISMATCH (geometry) tex=",
          reinterpret_cast<void*>(texture),
          " snapshotPitch=", snap.pitch, " realPitch=", pitch,
          " snapshotBytes=", snap.bytes.size(), " realBytes=", size);
  } else if (std::memcmp(real, snap.bytes.data(), size) != 0) {
    // Report where, not just that: the byte offset converts to an (x, y) in the
    // 512x512 atlas, which is what identifies the glyph that went stale.
    size_t first = 0;
    while (first < size && real[first] == snap.bytes[first])
      ++first;
    size_t differing = 0;
    for (size_t i = first; i < size; ++i)
      if (real[i] != snap.bytes[i])
        ++differing;
    const uint64_t n = verifyMismatches.fetch_add(1, std::memory_order_relaxed);
    if (n < kVerifyDetailLimit)
      log("ATLASVERIFY MISMATCH tex=", reinterpret_cast<void*>(texture),
          " firstByte=", first,
          " atlasXY=", first % pitch / 4, ",", first / pitch,
          " differingBytes=", differing, " of ", size,
          " renderTextDepth=", renderTextDepth);
    else if (n == kVerifyDetailLimit)
      log("ATLASVERIFY further mismatches suppressed; the per-frame counts"
          " continue");
  }

  originalAtlasUnlock(texture, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

uintptr_t hookedAtlasLock(uintptr_t texture, uintptr_t output,
                          uintptr_t level, uintptr_t mode) {
  uint16_t width = 0;
  uint16_t height = 0;
  if (texture) {
    const auto* bytes = reinterpret_cast<const BYTE*>(texture);
    std::memcpy(&width, bytes + 0x40, sizeof(width));
    std::memcpy(&height, bytes + 0x42, sizeof(height));
  }

  // Eligibility is the Arland rule: a 512x512 middleware texture, locked with a
  // real output pointer, while the verified text renderer is on the stack.
  const bool candidate = renderTextDepth && output &&
    width == 512 && height == 512;

  const bool cacheable = candidate && cacheActive &&
    atlasCacheArmed.load(std::memory_order_acquire);

  // Serve from an existing snapshot -- in BOTH access modes. The text renderer
  // maps the atlas for writing to rasterize a glyph and then maps a staging copy
  // for reading to blit it back, and both name the same middleware object, so one
  // snapshot is a coherent stand-in for the whole round trip. That is where the
  // saving comes from; serving only reads would leave most of it on the table.
  if (cacheable) {
    uintptr_t hitPitch = 0;
    std::shared_ptr<AtlasRead> toVerify;
    {
      std::lock_guard lock(atlasMutex);
      const auto found = atlasReads.find(texture);
      if (found != atlasReads.end() && found->second &&
          !found->second->bytes.empty()) {
        atlasCacheHits.fetch_add(1, std::memory_order_relaxed);
        *reinterpret_cast<void**>(output) = found->second->bytes.data();
        // Verify before the write marks the snapshot as diverged, and only while
        // it is still supposed to match the real texture.
        if (verifyActive && !found->second->absorbedWrite)
          toVerify = found->second;
        if (mode)
          found->second->absorbedWrite = true;
        syntheticAtlasLocks.push_back({ texture, found->second });
        hitPitch = found->second->pitch;
      }
    }
    if (hitPitch) {
      // Outside the mutex: this issues a real middleware lock, and holding the
      // cache lock across it would serialize every other thread behind a ~1 MB
      // comparison. The shared_ptr keeps the buffer alive regardless.
      if (toVerify)
        verifySnapshot(texture, *toVerify, height);
      return hitPitch;
    }
  }

  if (cacheable)
    atlasRealLocks.fetch_add(1, std::memory_order_relaxed);

  // A real write lock reaching the middleware while a snapshot for this texture
  // is live can only be a write the cache did not serve. See
  // noteForeignWriteOverLiveSnapshot.
  if (verifyActive && mode && output && width == 512 && height == 512) {
    verifyWritesExamined.fetch_add(1, std::memory_order_relaxed);
    bool live = false;
    {
      std::lock_guard lock(atlasMutex);
      const auto found = atlasReads.find(texture);
      live = found != atlasReads.end() && found->second &&
             !found->second->bytes.empty();
    }
    if (live)
      noteForeignWriteOverLiveSnapshot(texture);
  }

  const uintptr_t pitch = originalAtlasLock(texture, output, level, mode);

  // Snapshot only from a READ lock. A write mapping is discard-mapped, so its
  // contents are undefined on entry; snapshotting one captures uninitialized
  // memory and the read that follows is served that garbage. The Arland project
  // hit exactly this as a striped glyph, and the first candidate lock of each
  // atlas is a write, so the poisoning would happen at snapshot birth.
  const bool isReadLock = uint32_t(mode) == 0;
  if (cacheable && isReadLock && pitch && pitch <= 16384) {
    const void* mapped = *reinterpret_cast<void* const*>(output);
    const size_t size = size_t(pitch) * height;
    if (mapped && size <= 8 * 1024 * 1024) {
      auto entry = std::make_shared<AtlasRead>();
      entry->pitch = uint32_t(pitch);
      entry->bytes.resize(size);
      std::memcpy(entry->bytes.data(), mapped, size);
      std::lock_guard lock(atlasMutex);
      atlasReads[texture] = std::move(entry);
    }
  }
  if (cacheable && pitch)
    realCandidateAtlasLocks.push_back(texture);

  return pitch;
}

uintptr_t hookedAtlasUnlock(uintptr_t texture, uintptr_t b, uintptr_t c,
                             uintptr_t d) {
  if (cacheActive) {
    // Our own synthetic lock: there is no real mapping to release, so suppress
    // the call entirely rather than handing the middleware a pointer it never
    // issued.
    if (!syntheticAtlasLocks.empty() &&
        syntheticAtlasLocks.back().texture == texture) {
      syntheticAtlasLocks.pop_back();
      return 0;
    }
    if (!realCandidateAtlasLocks.empty() &&
        realCandidateAtlasLocks.back() == texture) {
      realCandidateAtlasLocks.pop_back();
    } else if (texture) {
      // An unlock that cannot be paired with the top of either per-thread stack
      // is outside what the cache can prove. Conservatively discard that
      // texture's snapshot so a later read cannot receive bytes whose mapping
      // history is unknown.
      //
      // Both stacks are matched at the TOP only, so an unlock that interleaves
      // rather than nests also lands here even though the lock was ours. The
      // extra miss costs performance, but retaining an unproven snapshot risks
      // corrupt text.
      std::lock_guard lock(atlasMutex);
      atlasReads.erase(texture);
    }
  }

  return originalAtlasUnlock(texture, b, c, d);
}

uintptr_t hookedRenderText(uintptr_t a, uintptr_t b, uintptr_t c,
                           uintptr_t d) {
  ++renderTextDepth;
  const uintptr_t result = originalRenderText(a, b, c, d);
  --renderTextDepth;
  return result;
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

bool installAtlasHooks(BYTE* base, const PhyreGame& game) {
  auto* render = base + game.renderTextRva;
  auto* lock = base + game.atlasLockRva;
  auto* unlock = base + game.atlasUnlockRva;

  if (!matches(render, kRenderTextExpected) ||
      !matches(lock, kAtlasLockExpected) ||
      !matches(unlock, game.unlockExpected)) {
    log("atlas fix: prologue verification failed, installing nothing");
    return false;
  }

  if (!installMinHookDetour(unlock, reinterpret_cast<void*>(&hookedAtlasUnlock),
                            reinterpret_cast<void**>(&originalAtlasUnlock)))
    return false;
  if (!installMinHookDetour(lock, reinterpret_cast<void*>(&hookedAtlasLock),
                            reinterpret_cast<void**>(&originalAtlasLock)))
    return false;
  if (!installMinHookDetour(render, reinterpret_cast<void*>(&hookedRenderText),
                            reinterpret_cast<void**>(&originalRenderText)))
    return false;

  log("atlas fix: installed on ", game.executable,
      " base=", reinterpret_cast<void*>(base),
      " renderText=", reinterpret_cast<void*>(uintptr_t(game.renderTextRva)),
      " atlasLock=", reinterpret_cast<void*>(uintptr_t(game.atlasLockRva)),
      " atlasUnlock=", reinterpret_cast<void*>(uintptr_t(game.atlasUnlockRva)));
  return true;
}

}  // namespace

namespace dusk {

// Repeated from the anonymous namespace above on purpose, and not redundant.
// MSVC's <cmath> declares ::log, so at any scope that reaches global scope the
// name is ambiguous between it and atfix::log. Inside the anonymous namespace
// the using-declaration there wins before lookup gets that far; out here it does
// not, so this namespace needs its own. MinGW does not declare ::log the same
// way, which is why the local cross-build compiles this file and CI does not.
using atfix::log;

bool installAtlasFix(BYTE* base, const PhyreGame& game, bool cache,
                     bool verify) {
  const bool ok = installAtlasHooks(base, game);
  // Only arm behaviour once every hook is in. A partial install leaves both
  // flags false, so any hook that did land remains pass-through.
  cacheActive = ok && cache;
  // The verifier checks what the cache serves, so there is nothing to check
  // without it.
  verifyActive = cacheActive && verify;
  if (verify && !verifyActive)
    log("atlas fix: verify needs DUSK_ATLAS_CACHE=1; leaving it off");
  if (verifyActive)
    log("atlas fix: VERIFY MODE -- snapshots are compared against the real atlas"
        " before being served, and writes the cache did not serve are reported."
        " Expect the game to run slowly; this is a correctness check, not a"
        " performance configuration. A running tally is logged every few hundred"
        " frames: a clean session is one where checks and writesExamined are"
        " LARGE and mismatches and foreignWrites are both zero. Both counters"
        " only advance while text is actually being rendered, so a session spent"
        " on a static screen proves nothing however long it lasts.");
  log("atlas fix: ", ok ? "installed" : "FAILED",
      " cache=", cacheActive ? 1 : 0,
      " verify=", verifyActive ? 1 : 0,
      " lifetime=frame");
  return ok;
}

constexpr uint64_t kCacheFirstReportFrame = 300;
constexpr uint64_t kCacheReportInterval = 3600;

void atlasFixFrameTick() {
  if (cacheActive) {
    // The frame boundary IS the cache lifetime. Discarding every snapshot here
    // is what keeps a mutable atlas from being served stale indefinitely, and it
    // is the deliberate safety limit the Arland project settled on rather than
    // trying to prove invalidation coverage across frames.
    std::lock_guard lock(atlasMutex);
    atlasReads.clear();
    atlasCacheArmed.store(true, std::memory_order_release);
  }

  ++frameIndex;

  // The first report is unconditional: the hit rate is the shipping fix's
  // headline measurement, and one line of it is what a bug report needs. The
  // repeats are a periodic counter, so they follow verbose logging.
  if (cacheActive &&
      (frameIndex == kCacheFirstReportFrame ||
       (atfix::verboseLogging() && frameIndex % kCacheReportInterval == 0))) {
    const uint64_t hits = atlasCacheHits.load(std::memory_order_relaxed);
    const uint64_t realLocks = atlasRealLocks.load(std::memory_order_relaxed);
    const uint64_t total = hits + realLocks;
    const uint64_t hitRatePermille = total ? hits * 1000 / total : 0;
    log("atlas cache: frame=", frameIndex,
        " hits=", hits,
        " realLocks=", realLocks,
        " hitRatePermille=", hitRatePermille);
  }

  // "No findings" is only meaningful next to how many checks produced it. A
  // session that reported nothing cannot be distinguished from one where the
  // verifier never reached the cache.
  if (verifyActive && (frameIndex == kVerifyFirstReportFrame ||
                       frameIndex % kVerifyReportInterval == 0)) {
    const uint64_t checks = verifyChecks.load(std::memory_order_relaxed);
    const uint64_t mismatches =
      verifyMismatches.load(std::memory_order_relaxed);
    const uint64_t foreign =
      verifyForeignWrites.load(std::memory_order_relaxed);
    log("ATLASVERIFY frame=", frameIndex, " checks=", checks,
        " writesExamined=",
        verifyWritesExamined.load(std::memory_order_relaxed),
        " mismatches=", mismatches, " foreignWrites=", foreign,
        checks == 0
          ? "  <- NOTHING VERIFIED YET; if this persists the check is not"
            " reaching the cache and the run proves nothing"
          : (mismatches == 0 && foreign == 0 ? "  (clean so far)" : "  <- SEE ABOVE"));
  }

}

}  // namespace dusk
