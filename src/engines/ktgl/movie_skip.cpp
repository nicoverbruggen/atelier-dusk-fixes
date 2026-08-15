// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// movie_skip.h; what is here is the per-build wiring and the notes that
// only mean anything beside the code they sit on.
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

// (wrapper, movieIndex, flag). Resolves the path and calls the play routine.
using MovieOpenProc = void (STDMETHODCALLTYPE*)(uintptr_t, int32_t, BYTE);
// (player, resolvedPath, flag). The one that actually opens the file.
using MoviePlayProc = void (STDMETHODCALLTYPE*)(uintptr_t, const char*, BYTE);

MovieOpenProc originalMovieOpen = nullptr;
MoviePlayProc originalMoviePlay = nullptr;

// One constant-index movie requester in each game -- the Title state's enter
// handler -- so one movie to skip.
constexpr int kStartupMovieBudget = 1;

// Resolved at install time; the hooks must not read the ini or the environment
// on the thread the engine calls them from.
std::atomic<bool> skipping{false};

// The index the path builder is currently resolving, -1 when none. Set around
// the forwarded call and cleared after it, so a play that arrives by any other
// route sees -1 and is never skipped. A plain atomic is enough because the two
// hooks are called synchronously on one thread, the builder's call to the play
// routine being the only path in.
std::atomic<int32_t> pendingIndex{-1};

// One line per distinct index, and nothing after the table is exhausted. This
// is what turns "the boot path should be asking for index 0" into a fact on the
// first run anyone makes, without asking them to set a switch first.
std::atomic<uint32_t> seenIndices{0};

void noteIndex(int32_t index, bool skipped, int ordinal) {
  if (index < 0 || index > 31)
    return;
  const uint32_t bit = 1u << index;
  const uint32_t previous = seenIndices.fetch_or(bit, std::memory_order_relaxed);
  if (previous & bit)
    return;
  // The ordinal is logged with the index because together they say whether the
  // budget is the right size: a second movie before the player has control
  // would mean one is too few.
  log("MOVIE: play #", std::dec, ordinal, " index=", index,
      skipped ? " (skipped)" : " (played)");
}

void STDMETHODCALLTYPE tracedMovieOpen(uintptr_t wrapper, int32_t index,
                                       BYTE flag) {
  pendingIndex.store(index, std::memory_order_relaxed);
  // Always forwarded. The gallery seen-bit is set inside here.
  originalMovieOpen(wrapper, index, flag);
  pendingIndex.store(-1, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE skippedMoviePlay(uintptr_t player, const char* path,
                                        BYTE flag) {
  const int32_t index = pendingIndex.load(std::memory_order_relaxed);
  int ordinal = 0;
  // The budget is consumed by every play, skipped or not, so a movie that
  // arrives before the boot one cannot leave it unprotected.
  const bool skip = skipping.load(std::memory_order_relaxed) &&
                    consumeStartupMovieBudget(kStartupMovieBudget, &ordinal);
  noteIndex(index, skip, ordinal);
  if (!skip) {
    originalMoviePlay(player, path, flag);
    return;
  }
  // Nothing to write. Returning here is the engine's own missing-file exit.
}

// Byte-identical in all four builds: this window carries no rip-relative or
// absolute operand.
//   mov r11, rsp / push rdi / sub rsp, 0x70 / mov [r11-0x58], -2
constexpr std::array<BYTE, 16> kMoviePlayExpected = {
  0x4c, 0x8b, 0xdc, 0x57, 0x48, 0x83, 0xec, 0x70,
  0x49, 0xc7, 0x43, 0xa8, 0xfe, 0xff, 0xff, 0xff,
};

// The path builder is the same instructions in both games and differs only in
// its frame size, so the window is per game and shared between that game's two
// builds -- the arrangement system_save_fix.cpp already uses.
//   mov rax, rsp / push rbp / push r14 / push r15 / lea rbp,[rax-0x5f] / sub rsp,N
constexpr std::array<BYTE, 16> kMovieOpenEschaExpected = {
  0x48, 0x8b, 0xc4, 0x55, 0x41, 0x56, 0x41, 0x57,
  0x48, 0x8d, 0x68, 0xa1, 0x48, 0x81, 0xec, 0xc0,
};
constexpr std::array<BYTE, 16> kMovieOpenShallieExpected = {
  0x48, 0x8b, 0xc4, 0x55, 0x41, 0x56, 0x41, 0x57,
  0x48, 0x8d, 0x68, 0xa1, 0x48, 0x81, 0xec, 0xb0,
};

}  // namespace

bool installKtglMovieSkip(BYTE* base, const KtglGame& game) {
  if (featureSupport(Feature::SkipIntroMovie) == Support::Unsupported) {
    log("FIXES intro_movie_skip=not_applicable");
    return false;
  }
  if (!featureEnabled(Feature::SkipIntroMovie)) {
    log("FIXES intro_movie_skip=off");
    return false;
  }
  if (!game.moviePlayRva || !game.movieOpenRva) {
    log("FIXES intro_movie_skip=no_row_for_this_build");
    return false;
  }

  const bool shallie = currentTitle() == Title::Shallie;

  BYTE* playTarget = base + game.moviePlayRva;
  BYTE* openTarget = base + game.movieOpenRva;
  const auto& openExpected =
    shallie ? kMovieOpenShallieExpected : kMovieOpenEschaExpected;

  if (!matches(playTarget, kMoviePlayExpected) ||
      !matches(openTarget, openExpected)) {
    log("Intro-movie-skip prologue mismatch play=0x", std::hex,
        game.moviePlayRva, " open=0x", game.movieOpenRva, std::dec,
        "; not installing");
    return false;
  }

  // Play first. `skipping` is only set once both hooks are live, so a partial
  // install leaves the shipped behaviour rather than a movie skipped without
  // knowing which one it was.
  if (!installMinHookDetour(playTarget,
                            reinterpret_cast<void*>(&skippedMoviePlay),
                            reinterpret_cast<void**>(&originalMoviePlay))) {
    log("FIXES intro_movie_skip=failed (play)");
    return false;
  }
  if (!installMinHookDetour(openTarget,
                            reinterpret_cast<void*>(&tracedMovieOpen),
                            reinterpret_cast<void**>(&originalMovieOpen))) {
    log("FIXES intro_movie_skip=failed (open); nothing is skipped");
    return false;
  }

  skipping.store(true, std::memory_order_relaxed);
  log("FIXES intro_movie_skip=active play=0x", std::hex, game.moviePlayRva,
      " open=0x", game.movieOpenRva, std::dec);
  return true;
}

}  // namespace atfix
