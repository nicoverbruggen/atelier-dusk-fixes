// SPDX-License-Identifier: MIT
//
// Intro movie skip for Escha & Logy and Shallie.
//
// NOT A PORT of the Ayesha one, though it ends up the same shape. That engine's
// `ThreadEasyRenderLogo` has no counterpart here and the movie path scheme is
// different (`Movie/`, `Movie_EN/Eng/`, `.pam` names rewritten to `.wmv`). What
// the two do share is the 11-entry, 0x20-byte movie table, which is why the
// gating looks familiar.
//
// THE SKIP REPRODUCES A PATH THE ENGINE ALREADY HAS, which is the whole safety
// argument and the reason this was not built until that path was found. The
// play routine begins:
//
//     cmp qword ptr [rcx], 0     ; no backend player at all
//     je  exit
//     ...build the resource request, open the file...
//     test rdi, rdi              ; the movie file did not open
//     je  exit
//     ...start the session...
//
// That second exit is the engine's own behaviour for a movie whose file is
// missing: it returns having touched nothing, the session state stays 0, the
// `isPlaying` predicate reports false, and the next `Movie::Update` tells its
// parent task the movie finished. The shipped games can reach it today -- the
// regional movie directories are not all complete -- so the surrounding code is
// written for it. The detour returns without calling the original, which lands
// the engine in exactly that state.
//
// WHY TWO HOOKS. The play routine receives the player and the resolved path,
// not the movie index, so it cannot tell the opening from an ending on its own.
// The index is one frame up, in the path builder. So the builder is hooked to
// note the index and the play routine is hooked to act on it. The builder is
// always called through, never skipped, and that is deliberate: the `bts` that
// sets the movie's gallery seen-bit lives INSIDE it. Skipping the builder would
// silently cost the player a gallery unlock; skipping only the play keeps it.
// (Ayesha differs here -- its caller sets the seen-bit before calling in, so
// there the open routine itself could be skipped.)
//
// The handoff is a plain atomic because the two are called synchronously on one
// thread, the builder's call to the play routine being the only path in.
//
// WHAT IS SKIPPED: the first movie the process plays, and nothing after. See
// consumeStartupMovieBudget in core/util.h for why the rule counts plays rather
// than reading the movie's index -- in short, the in-game Movies gallery goes
// through this same routine, and no identity rule can tell "the opening at boot"
// from "the opening the player just selected".
//
// One is the budget because these games have exactly one constant-index movie
// requester: the Title state's enter handler, which asks for index 0. Everything
// else -- endings, route intros, event movies -- comes from a script argument
// and happens long after the budget is spent. The index is still read and
// logged, because knowing which movie boots is worth having; it just does not
// decide anything.
//
// Every distinct index the play routine is asked for is logged once, so a run
// says what the boot path actually requests rather than leaving it inferred.
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
// route sees -1 and is never skipped.
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

  // Play first. With only this one live the index stays -1, isIntroMovie
  // refuses it and nothing is ever skipped -- so a partial install leaves the
  // shipped behaviour rather than a movie skipped without knowing which.
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
