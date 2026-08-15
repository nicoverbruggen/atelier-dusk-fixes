// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

// Intro movie skip for Escha & Logy and Shallie.
//
// NOT A PORT of the Ayesha one, though it ends up the same shape. That engine's
// `ThreadEasyRenderLogo` has no counterpart here and the movie path scheme is
// different (`Movie/`, `Movie_EN/Eng/`, `.pam` names rewritten to `.wmv`). What
// the two do share is the 11-entry, 0x20-byte movie table, which is why the
// gating looks familiar.
//
// THE SKIP REPRODUCES A PATH THE ENGINE ALREADY HAS, which is the whole safety
// argument. The
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
// The handoff between them is described beside the variable in movie_skip.cpp.
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
namespace atfix {

// Skip the movie played on the way to the title screen. Installs only when the
// capability matrix supports the feature and the user opted in. Returns true
// when both hooks are live.
bool installKtglMovieSkip(BYTE* base, const KtglGame& game);

}  // namespace atfix
