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

#include "loading_text_fix.h"
#include "../../core/game.h"
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
constexpr KtglGame kGames[] = {
  { "Atelier_Escha_and_Logy_EN.exe", 0x715e8c, 0x7ceed8, BuildEnglish },
  { "Atelier_Escha_and_Logy.exe",    0x73739c, 0x85e978, BuildMultilingual },
  { "Atelier_Shallie_EN.exe",        0x6bca4c, 0x76f320, BuildEnglish },
  { "Atelier_Shallie.exe",           0x6ff53c, 0x7c89c0, BuildMultilingual },
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

    installLoadingTextFix(id.base, *game);
    return true;
  }();
  return initialized;
}

void ktglFrameTick() {
}

}  // namespace dusk
