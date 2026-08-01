// SPDX-License-Identifier: MIT
//
// KTGL module. See ktgl.h: no fix is implemented for Escha & Logy or Shallie
// yet, so this only establishes identity.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "ktgl.h"

#include "../core/game.h"
#include "../core/hook_util.h"
#include "../core/log.h"

namespace atfix {
extern Log log;   // main.cpp
}

namespace {

using atfix::BuildEnglish;
using atfix::BuildMultilingual;
using atfix::ModuleIdentity;
using atfix::currentModuleIdentity;
using atfix::log;

// The four KTGL executables, fingerprinted the same way as Ayesha's two: name
// plus .text VirtualSize. SHA-256 for each is in WORK_DOC.md "Hook boundaries".
//
// A zero `textSize` would mean "name matches, size unknown" and is reported but
// never treated as a verified identity, so a future fix cannot install against
// an unrecognized build. None of the rows is zero now; the log line below still
// prints the size it saw, so a game patch that changes .text is visible in the
// log rather than silently accepted.
struct KtglGame {
  const char* executable;
  DWORD textSize;
  uint8_t exeBuild;
};

constexpr KtglGame kGames[] = {
  { "Atelier_Escha_and_Logy_EN.exe", 0x715e8c, BuildEnglish },
  { "Atelier_Escha_and_Logy.exe",    0x73739c, BuildMultilingual },
  { "Atelier_Shallie_EN.exe",        0x6bca4c, BuildEnglish },
  { "Atelier_Shallie.exe",           0x6ff53c, BuildMultilingual },
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
    log("ktgl: ", game->executable,
        " build=", game->exeBuild == BuildEnglish ? "EN" : "ML",
        " .text=", reinterpret_cast<void*>(uintptr_t(id.textSize)),
        game->textSize == 0
          ? " (not fingerprinted)"
          : (id.textSize == game->textSize ? " (verified)" : " (MISMATCH)"));
    log("FIXES ktgl=none (no fix implemented for this engine yet)");
    return true;
  }();
  return initialized;
}

void ktglFrameTick() {
}

}  // namespace dusk
