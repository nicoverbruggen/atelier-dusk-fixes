// SPDX-License-Identifier: MIT
//
// Intro movie skip for Ayesha.
//
// Ported from the Arland project's src/movie_skip.cpp, which is this project's
// own code (MIT). The game plays its movies through one open routine taking the
// player object and an index into a table of movies, 0x20 bytes per entry.
//
// THE SKIP DOES NOT INVENT A CODE PATH. The open routine already begins by
// asking whether the movie subsystem is ready, and when the answer is no it
// writes 1 to the player's state byte and returns without opening anything. The
// per-frame movie update reads that same byte first and reports "not playing",
// so the caller advances as though the movie had finished. That is the engine's
// own graceful degradation for a movie it cannot play, which means the
// surrounding code is already written to handle it. The detour reproduces
// exactly that: state byte to 1, no original call. Ayesha's not-ready branch is
// at EN 0x8f4f3 / ML 0x91c03 and its update early-out at EN 0x8f8d0, both read
// out of this build rather than assumed from Arland.
//
// SAVE DATA IS NOT AT RISK. The caller (EN 0x4ba5e0) sets the movie's
// seen-bit with `bts` into its bitset BEFORE calling the open routine, so
// detouring the open routine cannot lose an unlock.
//
// WHAT IS SKIPPED: the first movie the process plays, and nothing after. See
// consumeStartupMovieBudget in core/util.h for the reasoning, which is shared
// with the KTGL implementation so that the mod has one rule rather than two.
//
// The short version, and it is why this replaced an index-based gate: Ayesha's
// table has eleven entries -- `0 opening`, `1 ending`, `2 teaser`,
// `3 avantitle`, `4..10 worldview_01..07` -- and static analysis could not say
// which of `opening` and `avantitle` boots, because the play method is virtual
// and its callers pass a computed index. An index rule therefore had to skip
// both to be sure, and skipping both meant the in-game Movies gallery could not
// replay either. Counting plays needs to know neither: the budget is spent
// during boot, so the gallery is always afterwards.
//
// The index is still read and logged, because which movie boots is worth
// knowing; it just no longer decides anything.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "movie_skip.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/util.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// (player, movieIndex, flag, context). Returns nothing.
using MovieOpenProc = void (STDMETHODCALLTYPE*)(uintptr_t, int32_t, BYTE,
                                                uintptr_t);

// THIS IS THE FUNCTION BODY, NOT THE VTABLE SLOT -- see logo_skip.cpp on
// Ayesha's incremental-link thunks.
//
// Each address was anchored on its own build's single reference to the
// "Res/x64/movie/" path string. Ayesha splits the path build into a helper that
// holds the only reference to that string (EN 0x8fac0 / ML 0x921d0); the open
// routine is that helper's single caller. That is one hop longer than the Arland
// derivation, where the string is referenced from the open routine directly.
// The multilingual address is additionally a `homolog` MATCH from the English
// one, reverse vote confirming, both 0x355 bytes.
constexpr uintptr_t kMovieOpenRvas[2] = {
  /* BuildEnglish      */ 0x08f4a0,
  /* BuildMultilingual */ 0x091bb0,
};

// One movie is skipped: whatever the boot path plays first. Both tables were
// dumped and verified (EN 0x15f3a40, ML 0x1758470, eleven 0x20-byte records,
// identical between the builds).
constexpr int kStartupMovieBudget = 1;

// The player state byte the open routine sets when it declines to play.
constexpr uintptr_t kPlayerStateOffset = 0x30;

// Identical between Ayesha's two builds, and NOT the same window as either of
// the Arland ones: Ayesha frames with `lea rbp,[rsp-0x60]` (disp8) where
// Rorona and Meruru use disp32.
constexpr std::array<BYTE, 16> kMovieOpenExpected = {
  0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
  0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24,
};

MovieOpenProc originalMovieOpen = nullptr;

// Resolved once at install time; the hook must not read the ini or the
// environment on the thread the engine calls it from.
std::atomic<bool> skipping{false};

// One log line per distinct index, and nothing after the table is exhausted.
// This is what turns "we think the boot movie is index 0 or 3" into a fact on
// the first run somebody makes, without asking them to set a switch first.
std::atomic<uint32_t> seenIndices{0};

void noteIndex(int32_t index, bool skipped, int ordinal) {
  if (index < 0 || index > 31)
    return;
  const uint32_t bit = 1u << index;
  const uint32_t previous = seenIndices.fetch_or(bit, std::memory_order_relaxed);
  if (previous & bit)
    return;
  // The ordinal says whether the budget is the right size: a second movie
  // before the player has control would mean one is too few.
  log("MOVIE: open #", std::dec, ordinal, " index=", index,
      skipped ? " (skipped)" : " (played)");
}

void STDMETHODCALLTYPE skippedMovieOpen(uintptr_t self, int32_t index,
                                        BYTE flag, uintptr_t context) {
  int ordinal = 0;
  // The budget is consumed by every play, skipped or not, so a movie arriving
  // before the boot one cannot leave it unprotected.
  const bool skip = skipping.load(std::memory_order_relaxed) &&
                    consumeStartupMovieBudget(kStartupMovieBudget, &ordinal);
  noteIndex(index, skip, ordinal);
  if (!skip) {
    originalMovieOpen(self, index, flag, context);
    return;
  }
  *reinterpret_cast<BYTE*>(self + kPlayerStateOffset) = 1;
}

}  // namespace

bool installMovieSkip(BYTE* base, uint8_t exeBuild) {
  if (featureSupport(Feature::SkipIntroMovie) == Support::Unsupported) {
    log("FIXES intro_movie_skip=not_applicable");
    return false;
  }
  if (!featureEnabled(Feature::SkipIntroMovie)) {
    log("FIXES intro_movie_skip=off");
    return false;
  }

  const uintptr_t rva = kMovieOpenRvas[exeBuild == BuildEnglish ? 0 : 1];
  BYTE* target = base + rva;
  if (!matches(target, kMovieOpenExpected)) {
    log("Intro-movie-skip prologue mismatch rva=0x", std::hex, rva, std::dec,
        "; not installing");
    return false;
  }

  const bool installed = installMinHookDetour(
    target, reinterpret_cast<void*>(&skippedMovieOpen),
    reinterpret_cast<void**>(&originalMovieOpen));
  if (installed)
    skipping.store(true, std::memory_order_relaxed);
  log("FIXES intro_movie_skip=", installed ? "active" : "failed",
      " rva=0x", std::hex, rva, std::dec);
  return installed;
}

}  // namespace atfix
