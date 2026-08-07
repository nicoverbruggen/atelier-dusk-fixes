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
// WHICH INDICES ARE SKIPPED, and why this is not simply index 0 as it is in
// Arland. Ayesha's table has eleven entries where the Arland games have four:
//
//   0 opening    1 ending    2 teaser    3 avantitle    4..10 worldview_01..07
//
// "Avant title" is the pre-title sequence, so the movie a player actually sees
// when booting Ayesha may be index 3 rather than index 0. That could not be
// settled statically: the movie player's play method is virtual and its callers
// pass a computed index, so there is no constant-index call site to read. Both
// are skipped, because both are intro movies and the feature is worded as
// covering them. The endings and the seven worldview movies go through the same
// routine untouched, which is the whole reason this gates on the index at all.
//
// Every distinct index this routine is asked for is logged once, so the first
// real run says which movie the boot path actually plays and the set above can
// be narrowed if it turns out to be wider than it needs to be.
//
// THE MOVIES GALLERY IS AFFECTED. Everything that plays a movie goes through
// this one routine, so with the option on the opening cannot be replayed from
// the in-game gallery either. That is also true of the shipped Arland feature,
// where it is undocumented; here it is stated in ADVANCED.md.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "movie_skip.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"

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

// The movies played on the way to the title screen, and the player state byte
// the open routine sets when it declines to play. Entry 0 of the table was
// confirmed to be opening.wmv in both builds (EN table 0x15f3a40, ML 0x1758470).
constexpr int32_t kOpeningMovieIndex = 0;
constexpr int32_t kAvantTitleMovieIndex = 3;
constexpr uintptr_t kPlayerStateOffset = 0x30;

bool isIntroMovie(int32_t index) {
  return index == kOpeningMovieIndex || index == kAvantTitleMovieIndex;
}

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

void noteIndex(int32_t index, bool skipped) {
  if (index < 0 || index > 31)
    return;
  const uint32_t bit = 1u << index;
  const uint32_t previous = seenIndices.fetch_or(bit, std::memory_order_relaxed);
  if (previous & bit)
    return;
  log("MOVIE: open index=", std::dec, index, skipped ? " (skipped)" : " (played)");
}

void STDMETHODCALLTYPE skippedMovieOpen(uintptr_t self, int32_t index,
                                        BYTE flag, uintptr_t context) {
  const bool skip =
    skipping.load(std::memory_order_relaxed) && isIntroMovie(index);
  noteIndex(index, skip);
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
