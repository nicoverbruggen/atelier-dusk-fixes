// SPDX-License-Identifier: MIT
#pragma once
//
// The LTGL/KTGL module: Atelier Escha & Logy DX and Atelier Shallie DX.
//
// These two are UCRT builds on the newer Gust engine. They carry NO known menu
// hitch -- the Arland/Ayesha text renderer has no homolog in either binary and
// their queue drain mismatches on both vote and prologue shape (WORK_DOC.md
// 1.3) -- so nothing in `src/engines/phyre/` applies to them and none of it is reachable
// here. Their own open defects are different problems entirely:
//
//   * Escha & Logy: the shadow-texture bug AGT worked around by replacing the
//     SRV at init, and the system-save-data wipe on quit.
//   * Shallie: the CreateSamplerState bug AGT patched.
//
// None of those is implemented yet. What this module does install is the
// "Loadning system data." correction (loading_text_fix.h), which is the first
// fix here and needs no engine knowledge at all -- it rewrites one misspelled
// string literal in the mapped image. The four executable identities are
// verified; that check is the gate this and every future fix installs behind.
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
// declines; every RVA here is derived in WORK_DOC.md, "The 'Loadning system
// data.' typo", and none of them is currently zero.
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
  uint8_t exeBuild;
};

}  // namespace atfix

namespace dusk {

// Fingerprints the process against the known Escha & Logy / Shallie builds,
// records the result, and installs whichever fixes in this directory are
// enabled. Idempotent. Returns true if the executable was recognized, whether or
// not any individual fix installed -- each reports its own outcome to the log.
// Installs no hooks: nothing here detours anything yet.
bool initializeKtglFixes();

// Present. Nothing here needs a frame boundary yet; the hook exists so that the
// first fix which does can use it without touching the dispatch layer.
void ktglFrameTick();

}  // namespace dusk
