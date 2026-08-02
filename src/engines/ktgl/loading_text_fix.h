// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

// The "Loadning system data." typo, in Escha & Logy and in Shallie.
//
// THE DEFECT. Both games show a status line in the corner of the very first
// screen while they read the system save, and the English string is misspelled:
// "Loadning system data." It is one of the first things a player reads, it is in
// every one of the four KTGL executables (English and multilingual builds of
// both games), and it is the localization's mistake rather than the engine's --
// the sibling strings around it ("Saving system data.", "Deleting…") are
// correct, and the Japanese, Simplified and Traditional Chinese variants sitting
// beside it in the same table are all fine.
//
// WHERE IT LIVES. A plain narrow string literal in .rdata, reached by two or
// three `lea` sites in the save/load status code -- not a pointer table, and not
// a game data file. That is what makes this fixable at all: the bytes the game
// prints are the bytes in the mapped image, so correcting them in memory
// corrects the display, with no hook, no detour, and no per-frame cost.
//
// THE FIX. Overwrite the 22-byte literal in place with "Loading system data."
// and two NULs. The correct spelling is one character shorter, so it fits inside
// the region the compiler already reserved and nothing after it moves; the
// second NUL simply clears the byte the shortened string vacated. .rdata is
// mapped read-only, so the page is made writable for the write and its original
// protection restored immediately afterwards.
//
// NOTHING ON DISK IS TOUCHED. This is a runtime mutation of the loaded image,
// gated on the executable fingerprint and on a byte-for-byte check of the
// literal itself, exactly like every other patch in this project.
namespace dusk {

// Corrects the literal in the running image. Returns true only when the bytes
// were verified and rewritten. Declines, with a logged reason, when the feature
// is off, when the build has no address row, when the bytes at that address are
// not the shipped typo, or when the page cannot be made writable.
bool installLoadingTextFix(BYTE* base, const atfix::KtglGame& game);

}  // namespace dusk
