// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

// Intro movie skip for Ayesha.
//
// Ported from the Arland project's src/engines/phyre/movie_skip.h, which is this project's
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
namespace atfix {

// Skip the movies played on the way to the title screen. Installs only when the
// capability matrix supports the feature and the user opted in. `exeBuild` is
// the verified build the Phyre module already resolved. Returns true when the
// hook is live.
bool installMovieSkip(BYTE* base, uint8_t exeBuild);

}  // namespace atfix
