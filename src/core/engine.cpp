// SPDX-License-Identifier: MIT
//
// Engine dispatch. See engine.h for why the split exists and why it is still
// one DLL.
#include "engine.h"

#include <cstdlib>

#include "game.h"
#include "log.h"
#include "../engines/ktgl/ktgl.h"
#include "../engines/phyre/phyre.h"

namespace atfix {
extern Log log;   // main.cpp
}

namespace dusk {

namespace {

using atfix::Engine;
using atfix::currentEngine;
using atfix::currentTitle;
using atfix::engineName;
using atfix::log;
using atfix::titleName;

// Resolved once, on the first call, and never re-resolved: the engine follows
// from the executable, which cannot change under a running process.
Engine g_engine = Engine::Unknown;

}  // namespace

bool initializeEngineFixes() {
  static const bool initialized = [] {
    const Engine engine = currentEngine();
    log("Engine: ", engineName(engine),
        " (", titleName(currentTitle()), ")");
    // DUSK_DISABLE stands the whole mod down for one launch: Direct3D is still
    // forwarded, but no module initializes and nothing is hooked. It is what
    // the launcher's "Play without the mod" button passes to the game, so the
    // shipped behaviour can be compared against without moving files out of the
    // folder and having to remember to move them back.
    if (const char* disabled = std::getenv("DUSK_DISABLE")) {
      if (disabled[0] != '0') {
        log("DUSK_DISABLE is set: installing nothing, forwarding Direct3D only");
        return false;
      }
    }
    switch (engine) {
      case Engine::Phyre:
        if (!initializePhyreFixes())
          return false;
        break;
      case Engine::Ktgl:
        if (!initializeKtglFixes())
          return false;
        break;
      default:
        // Not one of the three Dusk executables. The proxy still forwards
        // Direct3D, which is what keeps a misplaced DLL harmless rather than
        // fatal.
        return false;
    }
    g_engine = engine;
    return true;
  }();
  return initialized;
}

void engineFrameTick() {
  switch (g_engine) {
    case Engine::Phyre: phyreFrameTick(); break;
    case Engine::Ktgl:  ktglFrameTick();  break;
    default: break;
  }
}

}  // namespace dusk
