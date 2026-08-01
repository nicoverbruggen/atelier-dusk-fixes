// SPDX-License-Identifier: MIT
//
// Ayesha font-atlas read cache, plus the diagnostic that justified it.
//
// Both ride the same four hooked entry points; the addresses and how they were
// derived are recorded in WORK_DOC.md "Hook boundaries". Do not change an RVA
// here without updating that table.
//
// DUSK_ATLAS_CACHE (the fix) serves repeated 512x512 font-atlas locks from a CPU
// snapshot. DUSK_ATLAS_STATS (the diagnostic) counts without changing behaviour;
// the two are independent and can run together, which is how a run shows the
// collapse rather than just asserting it. DUSK_ATLAS_TRACE adds the raw
// lock/unlock sequence of a single steady-state frame, which is the only thing
// that can settle the pairing question the aggregate counts raise.
//
// Lifetime is FRAME-SCOPED, i.e. the Arland Rorona path rather than the
// Totori/Meruru one. That is a measured choice, not a default: 72% of Ayesha's
// candidate locks arrive outside the resource-event queue drain (WORK_DOC.md
// 2.3), so a queue-scoped cache would miss most of the work. Snapshots are
// therefore discarded at Present, and never held across a frame boundary.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "atlas_fix.h"
#include "phyre.h"
#include "../core/hook_util.h"
#include "../core/log.h"
#include "../core/util.h"

namespace atfix {
extern Log log;
}

namespace {

using atfix::PhyreGame;
using atfix::matches;
using atfix::installMinHookDetour;
using atfix::log;

// Signatures match the Arland project's, which is expected: these are the same
// middleware and text-renderer functions (WORK_DOC.md "Corroborating the atlas
// lock past its WEAK verdict").
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

// Prologues verified in both Ayesha binaries; see WORK_DOC.md "Hook
// boundaries" for how each was derived.
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
// in the PhyreGame row in phyre.cpp rather than here (Arland precedent:
// dtorExpectedEn/Multi).

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

  // Wall time actually spent inside the hooked entry points, measured across the
  // whole hook body so a cache-served lock is charged its real (small) cost
  // rather than being invisible. Without these the out-of-drain traffic had
  // counts but no cost, and neither remaining fix could be sized: scaling the
  // in-drain per-lock rate implied ~14 ms/frame against an observed 12.3 ms
  // frame interval, i.e. the extrapolation was unsound and had to be replaced by
  // direct measurement.
  //
  // renderNanos is inclusive of the locks made beneath it, so
  // (renderNanos - lockNanos) is the CPU-side glyph and layout work above the
  // atlas -- exactly the part a text-bitmap replay cache would remove and this
  // atlas cache cannot.
  uint64_t lockNanos = 0;
  uint64_t renderNanos = 0;

  // Why the cache missed, and whether a snapshot ever stopped existing. These
  // were added to settle what the 27 real locks per steady-state frame are,
  // against three atlases and a frame-scoped lifetime that should need three.
  //
  // Measured on the English build, and identical on 136 of 147 reported frames:
  // missRead=3, missWrite=24, snapshotDrops=0. So the read side is exactly at
  // its floor -- one first touch per atlas after the frame boundary cleared it
  // -- and no snapshot is ever invalidated. There is no churn.
  //
  // The 24 are structural. A write mapping is discard-mapped, so a write miss
  // cannot take a snapshot, and the frame opens with a long run of write locks
  // before the first read arrives to create one. Every write in that run is a
  // real WRITE_DISCARD map of a 512x512 dynamic texture -- not a snapshot
  // rebuild; only the 3 reads copy anything.
  //
  // Keep these counters: snapshotDrops is the cheap regression check that the
  // invalidation rule has not started firing, which is what a change to the
  // lock/unlock pairing would show up as first.
  uint64_t missRead = 0;         // cacheable read lock with no snapshot to serve
  uint64_t missWrite = 0;        // cacheable write lock with no snapshot to serve
  uint64_t unmatchedUnlocks = 0; // unlock matching neither of this thread's stacks
  uint64_t snapshotDrops = 0;    // ...and a live snapshot was discarded for it

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
bool cacheActive = false;
bool traceActive = false;
bool verifyActive = false;
bool censusActive = false;

// ---------------------------------------------------------------------------
// Sequence trace (DUSK_ATLAS_TRACE)
// ---------------------------------------------------------------------------
//
// The counters above say how often a snapshot is dropped. They cannot say why,
// because the answer is an ordering property: the cache recognizes its own
// unlocks by matching the top of a per-thread stack, a LIFO pairing inherited
// from Arland's one-write-plus-one-read-per-glyph pattern. Ayesha issues two
// writes per read (WORK_DOC.md "Repeated font-atlas reads"), so that
// assumption may simply not hold here -- and no aggregate count can distinguish
// "the pairing is wrong" from "something else unlocks these textures".
//
// So this records the raw lock/unlock sequence of a single steady-state frame
// and prints it. One frame is enough: the steady-state frame is byte-identical
// across 84 of 95 frames, which is what makes it worth reading literally.
//
// Restricted to out-of-drain traffic, which is where the churn is (72% of locks,
// and the ~23 us/lock cost against ~1.25 us in-drain).

enum : uint8_t {
  kEvRenderEnter,
  kEvRenderExit,
  kEvLock,
  kEvUnlock,
};

// Lock outcomes.
enum : uint8_t {
  kLockHit,          // served from an existing snapshot
  kLockRealSnap,     // went to the middleware, and a snapshot was taken
  kLockReal,         // went to the middleware, no snapshot (write mode, or unarmed)
  kLockNonCandidate, // not a 512x512 atlas lock under the text renderer
};

// Unlock outcomes.
enum : uint8_t {
  kUnlockSynthetic,  // ours, suppressed
  kUnlockMatched,    // ours, a real lock we let through
  kUnlockDropped,    // unmatched, and a live snapshot was discarded
  kUnlockUnmatched,  // unmatched, but there was nothing to discard
};

struct TraceEvent {
  uint8_t kind = 0;
  uint8_t slot = 0xff;    // texture index in first-seen order
  uint8_t mode = 0xff;    // middleware access mode, locks only
  uint8_t outcome = 0;
  uint16_t depth = 0;     // renderTextDepth on entry
};

constexpr size_t kTraceCapacity = 2048;
// Steady state is ~266 events; anything wildly larger is a menu build and not
// the frame under study, so the trace is discarded rather than truncated.
std::vector<TraceEvent> traceEvents;    // guarded by countersMutex
bool traceOverflowed = false;
bool traceDumped = false;
uint64_t frameIndex = 0;

// Textures keep stable addresses, so a first-seen index is a stable name for the
// whole session and makes the dump readable as A/B/C rather than as pointers.
// Reset together with the event ring: the slot table is small, and non-candidate
// textures claim slots too, so letting it survive the frames the trace discards
// exhausts it before the dumped frame is reached and every token prints as `?`.
// That is exactly what the first run did.
uintptr_t traceSlots[8] = {};
uint8_t slotCount = 0;

void traceResetLocked() {
  traceEvents.clear();
  traceOverflowed = false;
  slotCount = 0;
}

uint8_t traceSlotFor(uintptr_t texture) {
  for (uint8_t i = 0; i < slotCount; ++i)
    if (traceSlots[i] == texture)
      return i;
  if (slotCount >= 8)
    return 0xff;
  traceSlots[slotCount] = texture;
  return slotCount++;
}

// Recorded only outside the queue drain, and only while the trace is still
// looking for its frame. Caller must NOT hold countersMutex.
void traceRecord(uint8_t kind, uintptr_t texture, uint8_t mode,
                 uint8_t outcome, unsigned depth) {
  if (!traceActive || queueDrainDepth)
    return;
  std::lock_guard lock(countersMutex);
  if (traceDumped)
    return;
  if (traceEvents.size() >= kTraceCapacity) {
    traceOverflowed = true;
    return;
  }
  traceEvents.push_back({ kind, texture ? traceSlotFor(texture) : uint8_t(0xff),
                          mode, outcome, uint16_t(depth) });
}

// Cache hit/miss counters, reported alongside the diagnostic so a run shows the
// collapse instead of asserting it.
std::atomic<uint64_t> atlasCacheHits{0};
std::atomic<uint64_t> atlasRealReads{0};

uint32_t packDims(uint16_t w, uint16_t h) {
  return (uint32_t(w) << 16) | uint32_t(h);
}

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

// `timeLabel` names what `micros` measures: the drain's own duration for an
// in-drain report, the Present-to-Present interval for a per-frame one.
void logCounters(const char* scope, const Counters& c, const char* timeLabel,
                 uint64_t micros) {
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
  // renderNanos is inclusive of the locks beneath it; the difference is the
  // CPU-side glyph and layout work above the atlas, which no atlas cache can
  // remove and a replay cache would.
  const uint64_t lockMicros = c.lockNanos / 1000;
  const uint64_t renderMicros = c.renderNanos / 1000;
  const uint64_t aboveAtlasMicros =
    c.renderNanos > c.lockNanos ? (c.renderNanos - c.lockNanos) / 1000 : 0;
  log(scope,
    ": ", timeLabel, "=", micros,
    " lockMicros=", lockMicros,
    " renderMicros=", renderMicros,
    " aboveAtlasMicros=", aboveAtlasMicros,
    " renderText=", c.renderTextCalls,
    " candidateLocks=", c.candidateLocks,
    " (read=", c.readLocks, " write=", c.writeLocks, ")",
    " nonCandidateLocks=", c.nonCandidateLocks,
    " unlocks=", c.unlocks,
    " distinctTextures=", c.perTexture.size(),
    " worstRepeat=", worstRepeat,
    " worstTexture=", reinterpret_cast<void*>(worstTexture));
  // Only when the cache is on: with it off every candidate lock is a "miss" by
  // definition and none of these means anything.
  if (cacheActive && (c.missRead || c.missWrite || c.unmatchedUnlocks))
    log("    churn: missRead=", c.missRead,
        " missWrite=", c.missWrite,
        " unmatchedUnlocks=", c.unmatchedUnlocks,
        " snapshotDrops=", c.snapshotDrops);
  for (const auto& entry : c.dimensions) {
    log("    dims ", (entry.first >> 16), "x", (entry.first & 0xffff),
        " locks=", entry.second);
  }
}

// Frames to let pass before the trace will accept one. Startup and the first
// menu builds are not the frame under study, and they are large enough to
// overflow the ring.
constexpr uint64_t kTraceWarmupFrames = 120;
constexpr size_t kTraceTokensPerLine = 24;

// Caller must hold countersMutex.
void dumpTraceLocked(uint64_t frame, size_t candidateLocks) {
  log("ATLASTRACE frame=", frame, " events=", traceEvents.size(),
      " candidateLocks=", candidateLocks);
  log("ATLASTRACE legend: [ ] = renderText enter/exit."
      "  Lock <tex><w|r><outcome>, outcome + hit, * real+snapshot,"
      " - real no snapshot, n non-candidate."
      "  Unlock <tex>u<outcome>, outcome s synthetic-suppressed, m matched,"
      " X SNAPSHOT DROPPED, . unmatched but nothing to drop.");
  std::string line;
  size_t inLine = 0;
  for (const TraceEvent& e : traceEvents) {
    const char slot = e.slot == 0xff ? '?' : char('A' + e.slot);
    char token[5] = {};
    switch (e.kind) {
      case kEvRenderEnter: token[0] = '['; break;
      case kEvRenderExit:  token[0] = ']'; break;
      case kEvLock:
        token[0] = slot;
        token[1] = e.mode ? 'w' : 'r';
        token[2] = e.outcome == kLockHit      ? '+'
                 : e.outcome == kLockRealSnap ? '*'
                 : e.outcome == kLockReal     ? '-'
                 :                              'n';
        break;
      default:
        token[0] = slot;
        token[1] = 'u';
        token[2] = e.outcome == kUnlockSynthetic ? 's'
                 : e.outcome == kUnlockMatched   ? 'm'
                 : e.outcome == kUnlockDropped   ? 'X'
                 :                                 '.';
        break;
    }
    line += token;
    line += ' ';
    if (++inLine == kTraceTokensPerLine) {
      log("ATLASTRACE   ", line);
      line.clear();
      inLine = 0;
    }
  }
  if (!line.empty())
    log("ATLASTRACE   ", line);
}

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
        " inDrain=", queueDrainDepth ? 1 : 0,
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
          " renderTextDepth=", renderTextDepth,
          " inDrain=", queueDrainDepth ? 1 : 0);
    else if (n == kVerifyDetailLimit)
      log("ATLASVERIFY further mismatches suppressed; the per-frame counts"
          " continue");
  }

  originalAtlasUnlock(texture, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Writer census (DUSK_ATLAS_CENSUS)
// ---------------------------------------------------------------------------
//
// The cache and the diagnostics above both apply the same eligibility filter
// (renderTextDepth && output) before they count anything, which is precisely
// what a census must not do: the open question is whether some other caller
// -- on another thread, or the game's own field/UI code reaching the
// middleware directly -- can write a font atlas outside that filter. Missing
// such a caller is what the invalidation rule in statsAtlasUnlock is a
// heuristic defense against; this instead tries to enumerate the callers
// directly, so the question can be closed by listing rather than by
// continuing to sample.
//
// Keyed on (caller return-address RVA, thread id, mode) rather than on
// texture: the RVA identifies the call site in the game's own code, which is
// the thing that answers "how many places can write here", while the texture
// pointer only says which atlas -- kept per tuple, capped, as corroboration.
//
// A fixed 32-entry table scanned linearly, not a hash map: this runs on every
// 512x512 lock, in-drain and out, so it has to stay cheap, and 32 distinct
// (site, thread, mode) tuples is already far more than the two known callers
// (write-then-read per glyph) on the one thread that has been observed. If it
// fills, that is itself the finding -- more distinct callers than assumed --
// and is reported as an overflow rather than silently dropping tuples.

struct CensusEntry {
  uintptr_t callerRva = 0;
  DWORD threadId = 0;
  uintptr_t mode = 0;
  uint64_t locks = 0;
  // Distinct textures seen for this tuple, first-seen order, capped at 8: this
  // is corroboration for the tuple, not the primary key, so a cap is fine and
  // keeps the entry small.
  uintptr_t textures[8] = {};
  uint8_t textureCount = 0;      // number actually stored, <= 8
  uint32_t distinctTextures = 0; // total distinct seen, uncapped
};

constexpr size_t kCensusCapacity = 32;
constexpr uint64_t kCensusReportInterval = 300;
// As with the verifier: an early report so a run confirms the instrument is
// live long before the first interval elapses.
constexpr uint64_t kCensusFirstReportFrame = 60;

atfix::mutex censusMutex;
CensusEntry censusEntries[kCensusCapacity];
size_t censusEntryCount = 0;
bool censusOverflowed = false;
uint64_t censusTotalLocks = 0;

// Module base for the RVA conversion, resolved once and cached: the base
// cannot change for the process lifetime, and GetModuleHandleW is not free
// enough to call on every 512x512 lock.
uintptr_t censusModuleBase() {
  static const uintptr_t base =
    reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  return base;
}

// Caller must NOT hold any other lock: this runs on every 512x512 lock,
// in-drain and out, and taking it under countersMutex or atlasMutex would
// serialize the census against work that has nothing to do with it.
void censusRecord(uintptr_t callerRva, uintptr_t texture, uintptr_t mode) {
  const DWORD threadId = GetCurrentThreadId();
  std::lock_guard lock(censusMutex);
  ++censusTotalLocks;
  for (size_t i = 0; i < censusEntryCount; ++i) {
    CensusEntry& e = censusEntries[i];
    if (e.callerRva != callerRva || e.threadId != threadId || e.mode != mode)
      continue;
    ++e.locks;
    bool seen = false;
    for (uint8_t j = 0; j < e.textureCount; ++j) {
      if (e.textures[j] == texture) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      ++e.distinctTextures;
      if (e.textureCount < 8)
        e.textures[e.textureCount++] = texture;
    }
    return;
  }
  if (censusEntryCount >= kCensusCapacity) {
    censusOverflowed = true;
    return;
  }
  CensusEntry& e = censusEntries[censusEntryCount++];
  e.callerRva = callerRva;
  e.threadId = threadId;
  e.mode = mode;
  e.locks = 1;
  e.textures[0] = texture;
  e.textureCount = 1;
  e.distinctTextures = 1;
}

std::string censusFormatTextures(const CensusEntry& e) {
  std::ostringstream oss;
  oss << '[';
  for (uint8_t j = 0; j < e.textureCount; ++j) {
    if (j)
      oss << ' ';
    oss << reinterpret_cast<void*>(e.textures[j]);
  }
  oss << ']';
  return oss.str();
}

// Caller must hold censusMutex.
void dumpCensusLocked(uint64_t frame) {
  size_t order[kCensusCapacity];
  for (size_t i = 0; i < censusEntryCount; ++i)
    order[i] = i;
  // Sorted so the highest-traffic caller -- the one most worth checking first
  // -- is always the first line, regardless of table insertion order.
  std::sort(order, order + censusEntryCount, [](size_t a, size_t b) {
    return censusEntries[a].locks > censusEntries[b].locks;
  });

  // Distinct callers and threads across the whole table, which is the number
  // that actually answers the census question; the per-tuple lines below are
  // the detail behind it.
  uintptr_t seenCallers[kCensusCapacity];
  DWORD seenThreads[kCensusCapacity];
  uint32_t distinctCallers = 0;
  uint32_t distinctThreads = 0;
  for (size_t i = 0; i < censusEntryCount; ++i) {
    const CensusEntry& e = censusEntries[i];
    bool foundCaller = false;
    for (uint32_t j = 0; j < distinctCallers; ++j) {
      if (seenCallers[j] == e.callerRva) {
        foundCaller = true;
        break;
      }
    }
    if (!foundCaller)
      seenCallers[distinctCallers++] = e.callerRva;

    bool foundThread = false;
    for (uint32_t j = 0; j < distinctThreads; ++j) {
      if (seenThreads[j] == e.threadId) {
        foundThread = true;
        break;
      }
    }
    if (!foundThread)
      seenThreads[distinctThreads++] = e.threadId;
  }

  if (censusEntryCount == 0) {
    log("ATLASCENSUS frame=", frame,
        " NOTHING RECORDED -- no 512x512 atlas lock has been seen at all."
        " Either no text has rendered yet, or the census is not reaching the"
        " lock hook; this run proves nothing until the count moves.");
    return;
  }
  log("ATLASCENSUS frame=", frame,
      " distinctCallers=", distinctCallers,
      " distinctThreads=", distinctThreads,
      " totalLocks=", censusTotalLocks,
      censusOverflowed
        ? "  <- TABLE OVERFLOWED: more than 32 distinct"
          " (callerRva,thread,mode) tuples exist; the lines below are a"
          " subset, and the overflow is itself the finding"
        : "");
  for (size_t i = 0; i < censusEntryCount; ++i) {
    const CensusEntry& e = censusEntries[order[i]];
    log("ATLASCENSUS callerRva=", reinterpret_cast<void*>(e.callerRva),
        " thread=", e.threadId,
        " mode=", e.mode,
        " locks=", e.locks,
        " textures=", e.distinctTextures,
        " ", censusFormatTextures(e));
  }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

uintptr_t atlasLockBody(uintptr_t texture, uintptr_t output,
                        uintptr_t level, uintptr_t mode,
                        uintptr_t callerRva) {
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

  // Deliberately NOT gated on `candidate`: the census exists to see the
  // callers that rule excludes (no output pointer, no text renderer on the
  // stack, a different thread), so its only filter is the dimension check.
  if (censusActive && width == 512 && height == 512)
    censusRecord(callerRva, texture, mode);

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
      traceRecord(kEvLock, texture, uint8_t(mode), kLockHit, renderTextDepth);
      return hitPitch;
    }
  }

  if (cacheable) {
    atlasRealReads.fetch_add(1, std::memory_order_relaxed);
    if (statsActive) {
      std::lock_guard lock(countersMutex);
      Counters& c = queueDrainDepth ? inDrain : outOfDrain;
      if (mode)
        ++c.missWrite;
      else
        ++c.missRead;
    }
  }

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
  bool snapshotTaken = false;
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
      snapshotTaken = true;
    }
  }
  if (cacheable && pitch)
    realCandidateAtlasLocks.push_back(texture);

  traceRecord(kEvLock, texture, uint8_t(mode),
              !candidate ? kLockNonCandidate
                         : (snapshotTaken ? kLockRealSnap : kLockReal),
              renderTextDepth);
  return pitch;
}

uintptr_t statsAtlasUnlock(uintptr_t texture, uintptr_t b, uintptr_t c,
                           uintptr_t d) {
  if (statsActive) {
    std::lock_guard lock(countersMutex);
    (queueDrainDepth ? inDrain : outOfDrain).unlocks++;
  }

  if (cacheActive) {
    // Our own synthetic lock: there is no real mapping to release, so suppress
    // the call entirely rather than handing the middleware a pointer it never
    // issued.
    if (!syntheticAtlasLocks.empty() &&
        syntheticAtlasLocks.back().texture == texture) {
      syntheticAtlasLocks.pop_back();
      traceRecord(kEvUnlock, texture, 0xff, kUnlockSynthetic, renderTextDepth);
      return 0;
    }
    if (!realCandidateAtlasLocks.empty() &&
        realCandidateAtlasLocks.back() == texture) {
      realCandidateAtlasLocks.pop_back();
      traceRecord(kEvUnlock, texture, 0xff, kUnlockMatched, renderTextDepth);
    } else if (texture) {
      // Any unlock that is not one of ours may be releasing a WRITE. The glyph
      // atlas is a single mutable, demand-paged surface, so the game rasterizes
      // fresh glyph pages into it mid-frame. Drop the snapshot on every such
      // unlock, or a glyph paged in after the snapshot is served from the stale
      // copy and blits blank -- the Arland missing-kanji bug.
      //
      // Both stacks are matched at the TOP only, so an unlock that interleaves
      // rather than nests lands here even though the lock was ours. That is the
      // suspected churn mechanism, and it is why `dropped` is counted apart from
      // the bare unmatched count: dropped-per-frame above three is a snapshot
      // that existed and was thrown away.
      bool dropped = false;
      {
        std::lock_guard lock(atlasMutex);
        dropped = atlasReads.erase(texture) != 0;
      }
      if (statsActive) {
        std::lock_guard lock(countersMutex);
        Counters& c = queueDrainDepth ? inDrain : outOfDrain;
        ++c.unmatchedUnlocks;
        if (dropped)
          ++c.snapshotDrops;
      }
      traceRecord(kEvUnlock, texture, 0xff,
                  dropped ? kUnlockDropped : kUnlockUnmatched, renderTextDepth);
    }
  }

  return originalAtlasUnlock(texture, b, c, d);
}

// Accumulate `nanos` into whichever scope this call belongs to. Kept separate
// from the body so the timing is charged once, at the hook boundary.
void chargeNanos(uint64_t Counters::*field, uint64_t nanos) {
  std::lock_guard lock(countersMutex);
  (queueDrainDepth ? inDrain : outOfDrain).*field += nanos;
}

uintptr_t statsAtlasLock(uintptr_t texture, uintptr_t output,
                         uintptr_t level, uintptr_t mode) {
  // Must be read here, in the hook entry point, and it does yield the game's
  // own call site rather than a MinHook stub. The chain the game takes is
  // `call <thunk>` -> `jmp <lock>` -> `jmp statsAtlasLock`: MinHook patches a
  // JUMP over the target's entry, and the incremental-link thunk is a jump too,
  // so there is exactly one CALL in the whole chain and the return address on
  // the stack is the one the game pushed. This is the same idiom the Arland
  // project uses to distinguish call sites of a shared hooked routine.
  //
  // atlasLockBody is an ordinary function called from here, so reading
  // duskReturnAddress() inside it would give this function's frame instead --
  // a constant, and useless as a census key.
  //
  // On the English build a closed result is two caller RVAs on one thread:
  // 0x74c025 (the read, inside renderText) and 0x5a9b29 (the write, via
  // 0x5aa770 -> 0x5a9ae0). The multilingual build has its own RVAs. Any third
  // caller, or any second thread, is the finding.
  const uintptr_t callerRva = censusActive
    ? uintptr_t(reinterpret_cast<uintptr_t>(duskReturnAddress()) -
                censusModuleBase())
    : 0;
  if (!statsActive)
    return atlasLockBody(texture, output, level, mode, callerRva);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result =
    atlasLockBody(texture, output, level, mode, callerRva);
  chargeNanos(&Counters::lockNanos, uint64_t(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - started).count()));
  return result;
}

uintptr_t statsRenderText(uintptr_t a, uintptr_t b, uintptr_t c,
                          uintptr_t d) {
  if (!statsActive) {
    ++renderTextDepth;
    const uintptr_t result = originalRenderText(a, b, c, d);
    --renderTextDepth;
    return result;
  }
  // Bracketing each render call is what makes the trace readable: it turns a
  // flat lock sequence into per-string groups, which is the level the
  // write/read pairing actually lives at.
  traceRecord(kEvRenderEnter, 0, 0xff, 0, renderTextDepth);
  {
    std::lock_guard lock(countersMutex);
    (queueDrainDepth ? inDrain : outOfDrain).renderTextCalls++;
  }
  // Only the outermost renderText is timed: nested calls would double-count into
  // the same total.
  const bool outermost = renderTextDepth == 0;
  const auto started = std::chrono::steady_clock::now();
  ++renderTextDepth;
  const uintptr_t result = originalRenderText(a, b, c, d);
  --renderTextDepth;
  if (outermost)
    chargeNanos(&Counters::renderNanos, uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count()));
  traceRecord(kEvRenderExit, 0, 0xff, 0, renderTextDepth);
  return result;
}

void statsQueueDrain(void* self) {
  // The drain deliberately does NOT arm or clear the cache. In the frame-scoped
  // lifetime that Ayesha measured into, the frame boundary owns both, and having
  // the drain also clear would throw away snapshots mid-frame -- discarding
  // exactly the reuse the out-of-drain majority depends on.
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
    logCounters("  inDrain", inDrain, "drainMicros", micros);
  }
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

bool installAtlasHooks(BYTE* base, const PhyreGame& game) {
  auto* queue = base + game.queueDrainRva;
  auto* render = base + game.renderTextRva;
  auto* lock = base + game.atlasLockRva;
  auto* unlock = base + game.atlasUnlockRva;

  if (!matches(queue, kQueueDrainExpected) ||
      !matches(render, kRenderTextExpected) ||
      !matches(lock, kAtlasLockExpected) ||
      !matches(unlock, game.unlockExpected)) {
    log("atlas fix: prologue verification failed, installing nothing");
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

  log("atlas fix: installed on ", game.executable,
      " base=", reinterpret_cast<void*>(base),
      " queueDrain=", reinterpret_cast<void*>(uintptr_t(game.queueDrainRva)),
      " renderText=", reinterpret_cast<void*>(uintptr_t(game.renderTextRva)),
      " atlasLock=", reinterpret_cast<void*>(uintptr_t(game.atlasLockRva)),
      " atlasUnlock=", reinterpret_cast<void*>(uintptr_t(game.atlasUnlockRva)));
  return true;
}

}  // namespace

namespace dusk {

bool installAtlasFix(BYTE* base, const PhyreGame& game, bool cache, bool stats,
                     bool trace, bool verify, bool census) {
  const bool ok = installAtlasHooks(base, game);
  // Only arm behaviour once every hook is in. A partial install leaves all three
  // flags false, so the hooks that did land stay pass-through.
  statsActive = ok && stats;
  cacheActive = ok && cache;
  // The trace picks its frame out of the diagnostic's per-frame counters and is
  // bracketed by the renderText hook's stats path, so it cannot run without it.
  traceActive = statsActive && trace;
  if (trace && !traceActive)
    log("atlas fix: trace needs DUSK_ATLAS_STATS=1; leaving it off");
  if (traceActive)
    traceEvents.reserve(kTraceCapacity);
  // The verifier checks what the cache serves, so there is nothing to check
  // without it. It does not need the diagnostic.
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
  // The census needs nothing else: it keys on the caller's own call site, not
  // on anything the cache or the other diagnostics compute, so it works with
  // the cache off just as well as on.
  censusActive = ok && census;
  if (censusActive)
    log("atlas fix: CENSUS MODE -- enumerating every 512x512 atlas lock by"
        " (caller RVA, thread, mode), regardless of output/renderTextDepth/"
        "drain state. Reported every ", kCensusReportInterval,
        " frames as ATLASCENSUS lines.");
  log("atlas fix: ", ok ? "installed" : "FAILED",
      " cache=", cacheActive ? 1 : 0,
      " stats=", statsActive ? 1 : 0,
      " trace=", traceActive ? 1 : 0,
      " verify=", verifyActive ? 1 : 0,
      " census=", censusActive ? 1 : 0, " lifetime=frame");
  return ok;
}

// Previous Present, for the frame interval. Only read under countersMutex.
std::chrono::steady_clock::time_point g_lastPresent;

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

  {
    std::lock_guard lock(countersMutex);
    ++frameIndex;
  }

  // Reported independently of DUSK_ATLAS_STATS. The verifier answers a
  // correctness question, so "no findings" is only meaningful next to a count of
  // how many checks produced it: a session that reported nothing at all cannot
  // be told apart from a session where the check never ran. The first run of
  // this mode was exactly that, because this line used to sit below the
  // statsActive guard.
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

  // Reported independently of DUSK_ATLAS_STATS, same reasoning as the
  // verifier above: the census answers "who can write a font atlas" on its
  // own terms and does not need the other diagnostic's counters to do it.
  if (censusActive && (frameIndex == kCensusFirstReportFrame ||
                       frameIndex % kCensusReportInterval == 0)) {
    std::lock_guard lock(censusMutex);
    dumpCensusLocked(frameIndex);
  }

  if (!statsActive)
    return;
  std::lock_guard lock(countersMutex);
  const auto now = std::chrono::steady_clock::now();
  // Frame interval, so the per-frame text cost can be read as a share of the
  // frame budget instead of in isolation. Zero on the first frame.
  const uint64_t frameMicros =
    g_lastPresent == std::chrono::steady_clock::time_point{}
      ? 0
      : uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
          now - g_lastPresent).count());
  g_lastPresent = now;

  // Pick the frame the trace prints. Steady state is the case under study, and
  // it is recognizable without guessing: a frame that rendered text, did not
  // render much of it, and is past startup. Everything else is discarded so the
  // ring is empty for the next candidate.
  if (traceActive && !traceDumped && !traceEvents.empty()) {
    if (frameIndex > kTraceWarmupFrames && !traceOverflowed &&
        outOfDrain.candidateLocks && outOfDrain.renderTextCalls <= 4) {
      dumpTraceLocked(frameIndex, size_t(outOfDrain.candidateLocks));
      traceDumped = true;
      log("ATLASTRACE done -- one frame only; restart the game for another");
    } else {
      traceResetLocked();
    }
  }

  // Locks outside any drain. In Ayesha this is the majority of them, which is
  // what selected the frame-scoped lifetime over the queue-scoped one.
  if (outOfDrain.candidateLocks || outOfDrain.renderTextCalls) {
    logCounters("outOfDrain (frame)", outOfDrain, "frameMicros", frameMicros);
    if (cacheActive)
      log("    cache: hits=", atlasCacheHits.load(std::memory_order_relaxed),
          " realReads=", atlasRealReads.load(std::memory_order_relaxed));
    // Session totals, so a clean run is one where mismatches stays 0 for the
    // whole log and checks is large enough to mean something.
    if (verifyActive)
      log("    verify: checks=", verifyChecks.load(std::memory_order_relaxed),
          " mismatches=", verifyMismatches.load(std::memory_order_relaxed),
          " foreignWrites=",
          verifyForeignWrites.load(std::memory_order_relaxed),
          " writesExamined=",
          verifyWritesExamined.load(std::memory_order_relaxed));
  }
  outOfDrain.reset();
}

}  // namespace dusk
