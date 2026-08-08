// SPDX-License-Identifier: MIT
#pragma once
//
// The LTGL/KTGL module: Atelier Escha & Logy DX and Atelier Shallie DX.
//
// These two are UCRT builds on the newer Gust engine. They carry NO known menu
// hitch -- the Arland/Ayesha text renderer has no homolog in either binary and
// their queue drain mismatches on both vote and prologue shape -- so nothing in
// `src/engines/phyre/` applies to them and none of it is reachable here. Their
// own open defects are different problems entirely:
//
//   * Escha & Logy: the shadow-texture bug AGT worked around by replacing the
//     SRV at init, and the system-save-data wipe on quit.
//   * Shallie: the CreateSamplerState bug AGT patched.
//
// Neither of those is implemented yet. What this module does install is the
// "Loadning system data." correction (loading_text_fix.h), the system-save
// guard, Shallie's control-prompt hold, the intro movie skip and the startup
// logo skip. The four executable identities are verified; that check is the
// gate this and every fix here installs behind.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>

namespace atfix {

// Per-executable descriptor for the four KTGL builds: the executable identity,
// fingerprinted the same way as Ayesha's two (name plus .text VirtualSize), plus
// the RVAs a fix in this directory needs.
//
// A zero `textSize` would mean "name matches, size unknown" and is reported but
// never treated as a verified identity, so a fix cannot install against an
// unrecognized build. A zero RVA means that fix has no row for this build and
// declines; every RVA here was derived separately, and none of them is
// currently zero.
struct KtglGame {
  const char* executable;
  DWORD textSize;
  uintptr_t loadingTextRva;       // the "Loadning system data." literal in .rdata
  uintptr_t systemLoadStepRva;    // PlatformSteam::Load::step  (system_save_fix.h)
  uintptr_t systemSaveStepRva;    // PlatformSteam::Save::step  (system_save_fix.h)
  uintptr_t controlPromptUpdateRva; // ButtonHelp::Update, Shallie only
  uintptr_t controlPromptDrawRva;   // ButtonHelp::Draw, Shallie only
  uintptr_t padCreateWrapperRva;  // the CS-guarded pad create (core/pad_rescan.h)
  // The pad wrapper's prologue is displacement-free for only twelve bytes and
  // then carries a per-build rip-relative operand, so this window cannot be
  // shared the way the save-path ones are.
  std::array<BYTE, 16> padCreateExpected;
  uintptr_t mixCardUpdateRva;     // Card::Update, the synthesis pump (core/mix_card.h)
  // The two halves of the intro movie skip (movie_skip.h). The play routine is
  // where the file is opened and where the skip happens; the path builder one
  // frame up is the only place carrying the movie index, and it is always
  // forwarded because the gallery seen-bit is set inside it.
  uintptr_t moviePlayRva;
  uintptr_t movieOpenRva;
  // The DUSK_IPU_TRACE diagnostic (ipu_trace.h): the 2D-image loader and the
  // table it indexes, whose rows name their own files. Both were taken from the
  // build's own RTTI -- the `Ipu` vtable, then the slot whose prologue matches
  // -- rather than by searching for a shape, which matters because the slot
  // differs between the games (Escha 3, Shallie 14). `Rows` is cross-checked at
  // install against the bound the loader itself carries; the counts differ
  // because Shallie's table is shorter.
  uintptr_t ipuLoadRva;
  uintptr_t ipuTableRva;
  uint32_t ipuTableRows;
  // The startup logo skip (logo_skip.h): the body all three logo states call,
  // and the byte offset of the elapsed-seconds float inside `Title` that the
  // body zeroes and the sequence's advance check reads. The offset differs
  // between the games because Shallie lays that object out differently, and the
  // install refuses unless the body it hooked really does zero the offset the
  // row names.
  uintptr_t logoEnterRva;
  uint32_t logoElapsedOffset;
  // Removing the hold is not enough on its own: the step opens with a fade and
  // closes with another, and its update refuses to advance while either runs. So
  // the skip also answers "is a fade running" with no, for the one image object
  // the logo step owns. `logoIpuOffset` is where the step stores that object on
  // the `Title`, and `ipuFadeBusyRva` is the predicate body -- a different vtable
  // slot in each game (Escha 8, Shallie 19), so it is carried as an address.
  uint32_t logoIpuOffset;
  uintptr_t ipuFadeBusyRva;
  // Byte on the image object that stops it drawing. Not invented here: the image
  // loader clears exactly this byte on its own "this row has no image" branch,
  // one instruction before it records -1 as the held row. The skip clears the
  // byte and leaves the row alone, which is the half of that branch that is safe
  // -- the -1 is what crashed the first attempt. Escha keeps it at 0xc0 and
  // Shallie at 0x40.
  uint32_t ipuHideOffset;
  uint8_t exeBuild;
};

}  // namespace atfix

namespace dusk {

// Fingerprints the process against the known Escha & Logy / Shallie builds,
// records the result, and installs whichever fixes in this directory are
// enabled. Idempotent. Returns true if the executable was recognized, whether or
// not any individual fix installed -- each reports its own outcome to the log.
bool initializeKtglFixes();

// Present. Nothing here needs a frame boundary yet; the hook exists so that the
// first fix which does can use it without touching the dispatch layer.
void ktglFrameTick();

}  // namespace dusk
