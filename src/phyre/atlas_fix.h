// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "phyre.h"

namespace dusk {

// Installs the four Ayesha font-atlas hooks on an already-recognized build, and
// arms whichever of the two consumers was asked for: `cache` is the shipping fix
// (DUSK_ATLAS_CACHE), `stats` the diagnostic that justified it
// (DUSK_ATLAS_STATS). They are independent and can run together, which is how a
// run shows the collapse rather than asserting it.
//
// Behaviour arms only if all four hooks install, so a partial install is
// pass-through rather than half-caching. Returns that outcome.
// `trace` additionally records one steady-state frame's raw lock/unlock
// sequence (DUSK_ATLAS_TRACE) and requires `stats`, which is what supplies the
// frame accounting it picks its frame from.
//
// `verify` (DUSK_ATLAS_VERIFY) compares each snapshot against the real atlas
// before serving it, for as long as that snapshot is supposed to match. It
// requires `cache` -- it checks what the cache serves -- and makes the game
// slow. It is the machine-checked answer to "would this have shown a wrong
// glyph", which a playthrough cannot give.
//
// `census` (DUSK_ATLAS_CENSUS) is independent of all of the above and needs
// none of them: it enumerates, by (caller RVA, thread, mode) tuple, every
// call to the atlas lock on a 512x512 texture, regardless of `output`,
// `renderTextDepth` or drain state -- the whole point is to see the callers
// the cache's own eligibility rule excludes. It exists to answer "who can
// write a font atlas, and from how many threads" by listing every caller
// rather than sampling one.
bool installAtlasFix(BYTE* base, const atfix::PhyreGame& game,
                     bool cache, bool stats, bool trace, bool verify,
                     bool census);

// Called from Present. This is the atlas cache's frame boundary: Ayesha measured
// as the frame-scoped (Rorona-class) case, so every snapshot is discarded here.
// It also closes out the diagnostic's per-frame counters.
void atlasFixFrameTick();

}  // namespace dusk
