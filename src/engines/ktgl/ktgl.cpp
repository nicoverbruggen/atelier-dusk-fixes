// SPDX-License-Identifier: MIT
//
// KTGL module entry point: executable recognition, the Escha & Logy / Shallie
// address pack, and the install order for the fixes in this directory. As in
// src/engines/phyre/phyre.cpp, the fixes themselves do not recognize the
// executable; they are handed the base and the verified descriptor from here.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "ktgl.h"

#include "control_prompt_fix.h"
#include "loading_text_fix.h"
#include "system_save_fix.h"
#include "../../core/game.h"
#include "../../core/pad_rescan.h"
#include "../../../vendor/minhook/include/MinHook.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"

namespace atfix {
extern Log log;   // main.cpp
}

namespace {

using atfix::BuildEnglish;
using atfix::BuildMultilingual;
using atfix::KtglGame;
using atfix::ModuleIdentity;
using atfix::currentModuleIdentity;
using atfix::log;

// The four KTGL executables, fingerprinted the same way as Ayesha's two: name
// plus .text VirtualSize. SHA-256 for each is in WORK_DOC.md "Hook boundaries";
// see KtglGame in ktgl.h for what a zero in either the size or an RVA means.
//
// The log line below prints the .text size it actually saw, so a game patch that
// changes .text is visible in the log rather than silently accepted.
//
// loadingTextRva is the "Loadning system data." literal in each build's .rdata,
// derived in WORK_DOC.md, "The 'Loadning system data.' typo". All four differ
// because these are four separate compiles.
// A zero in any RVA column means "this fix has no row for this build" and the
// fix declines. controlPrompt* is zero for both Escha builds because Escha does
// not have that panel at all -- not because the address is unknown.
constexpr KtglGame kGames[] = {
  { "Atelier_Escha_and_Logy_EN.exe", 0x715e8c, 0x7ceed8,
    0x138df0, 0x138a70, 0, 0, 0x5d0640,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0x98, 0x28, 0xaf, 0x00 },
    BuildEnglish },
  { "Atelier_Escha_and_Logy.exe",    0x73739c, 0x85e978,
    0x13fa60, 0x13f6e0, 0, 0, 0x5f1b70,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0x68, 0xef, 0xe7, 0x00 },
    BuildMultilingual },
  { "Atelier_Shallie_EN.exe",        0x6bca4c, 0x76f320,
    0x0c2670, 0x0c28d0, 0x48da0, 0x49550, 0x5d5170,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0x98, 0x78, 0xaf, 0x00 },
    BuildEnglish },
  { "Atelier_Shallie.exe",           0x6ff53c, 0x7c89c0,
    0x0c3ec0, 0x0c4120, 0x48b80, 0x49330, 0x617b60,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0xa8, 0xa1, 0xe0, 0x00 },
    BuildMultilingual },
};

}  // namespace

namespace dusk {

bool initializeKtglFixes() {
  static const bool initialized = [] {
    ModuleIdentity id;
    if (!currentModuleIdentity(id))
      return false;

    const KtglGame* game = nullptr;
    for (const KtglGame& candidate : kGames) {
      if (!_stricmp(id.name, candidate.executable)) {
        game = &candidate;
        break;
      }
    }
    if (!game) {
      log("ktgl: unrecognized executable ", id.name,
          " .text=", reinterpret_cast<void*>(uintptr_t(id.textSize)));
      return false;
    }

    // Printed on every run so a build's fingerprint can be captured without a
    // separate tool, and so a game patch that changes .text is visible in the
    // log rather than silently accepted.
    const bool verified = game->textSize != 0 && id.textSize == game->textSize;
    log("ktgl: ", game->executable,
        " build=", game->exeBuild == BuildEnglish ? "EN" : "ML",
        " .text=", reinterpret_cast<void*>(uintptr_t(id.textSize)),
        game->textSize == 0
          ? " (not fingerprinted)"
          : (verified ? " (verified)" : " (MISMATCH)"));
    // The name alone is not an identity. Every RVA in kGames was read out of one
    // specific compile, so a build whose .text has moved gets the log line above
    // and nothing else -- a patch that fires on an unrecognized build is worse
    // than no patch.
    if (!verified) {
      log("FIXES ktgl=none (executable is not a verified build; installing"
          " nothing)");
      return true;
    }

    // The loading-text correction rewrites bytes and hooks nothing, so it runs
    // before MinHook exists and would still work if MinHook failed entirely.
    installLoadingTextFix(id.base, *game);

    // Everything below detours something, and until this change nothing in this
    // module ever did -- so MinHook was never initialized on these two games and
    // every install here failed with MH_ERROR_NOT_INITIALIZED while reporting a
    // matched prologue. That is the same trap `hookFactoryForSwapChain` fell
    // into: the Phyre module was the only thing initializing MinHook, and it
    // does not load on KTGL. A second call answers MH_ERROR_ALREADY_INITIALIZED,
    // which is a success here.
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
      log("ktgl: MH_Initialize failed (", MH_StatusToString(init),
          "); installing no hooks");
      return true;
    }

    installSystemSaveFix(id.base, *game);
    installControlPromptFix(id.base, *game);
    // The pad rescan is engine-agnostic machinery with a per-executable address,
    // so core owns the mechanism and this module supplies the row. See
    // core/pad_rescan.h; the prologue is displacement-free for twelve bytes and
    // then per-build, which is why the window is stored here rather than shared.
    atfix::installPadRescanBackoff(id.base,
      { game->padCreateWrapperRva, game->padCreateExpected });
    return true;
  }();
  return initialized;
}

void ktglFrameTick() {
}

}  // namespace dusk
