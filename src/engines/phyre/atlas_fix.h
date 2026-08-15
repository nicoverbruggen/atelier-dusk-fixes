// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "phyre.h"

namespace dusk {

// Installs the three Ayesha font-atlas hooks on an already-recognized build and
// arms the shipping cache when requested. The render-text hook identifies atlas
// work, the lock hook serves or creates snapshots, and the unlock hook suppresses
// synthetic unlocks or invalidates snapshots whose mapping history is unknown.
//
// Behaviour arms only if all three hooks install, so a partial install is
// pass-through rather than half-caching. Returns that outcome.
//
// WHAT IT IS WORTH, measured rather than assumed: the pattern it addresses is
// 2385 candidate locks onto 3 atlases per 248 ms drain, plus a per-frame
// steady-state drip. Caching them cuts menu-build time by 85% at a 95.5% hit
// rate. That measurement is why the cache ships on by default.
//
// `verify` (DUSK_ATLAS_VERIFY) compares each snapshot against the real atlas
// before serving it, for as long as that snapshot is supposed to match. It
// requires `cache` -- it checks what the cache serves -- and makes the game
// slow on purpose, at a real atlas lock plus a ~1 MB comparison per verified
// read. It is the machine-checked answer to "would this have shown a wrong
// glyph", which a playthrough cannot give: a stale glyph in Japanese is not
// something a reader can reliably spot.
//
bool installAtlasFix(BYTE* base, const atfix::PhyreGame& game,
                     bool cache, bool verify);

// Called from Present. This is the atlas cache's frame boundary: Ayesha measured
// as the frame-scoped (Rorona-class) case, so every snapshot is discarded here.
// It also emits a sparse session-health line for the cache and any enabled
// verifier report.
void atlasFixFrameTick();

}  // namespace dusk
