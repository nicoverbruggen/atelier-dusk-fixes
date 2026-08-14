// SPDX-License-Identifier: MIT
//
// PhyreEngine module entry point: executable recognition, the Ayesha address
// pack, and the install order for the fixes in this directory. The fixes
// themselves live alongside -- atlas_fix.cpp, field_physics.cpp -- and none of
// them recognizes the executable itself; they are handed the base and the
// verified descriptor from here.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "phyre.h"

#include "atlas_fix.h"
#include "field_physics.h"
#include "logo_skip.h"
#include "movie_skip.h"
#include "scene_target.h"
#include "worldmap_fix.h"
#include "../../core/game.h"
#include "../../core/pad_notify_trace.h"
#include "../../core/pad_rescan.h"
#include "../../core/log.h"
#include "../../../vendor/minhook/include/MinHook.h"

namespace atfix {
extern Log log;   // main.cpp
}

namespace {

using atfix::BuildEnglish;
using atfix::BuildMultilingual;
using atfix::ModuleIdentity;
using atfix::PhyreGame;
using atfix::currentModuleIdentity;
using atfix::log;

// The two Ayesha builds. Each row carries the executable's exact .text size and
// the RVAs derived from that build. The unlock prologue is per-row because the
// hooked unlock is a jmp stub whose 16-byte window contains a rel32
// displacement; the render and lock windows are build-independent and live
// beside their hooks in atlas_fix.cpp.
constexpr PhyreGame kGames[] = {
  { "Atelier_Ayesha_EN.exe", 0x984df4,
    0x74bd90, 0x581420, 0x581460,
    { 0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x01, 0xff,
      0xa7, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    0x584fb0,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0x80, 0x51, 0x4e, 0x01 },
    BuildEnglish },
  { "Atelier_Ayesha.exe", 0x9a9604,
    0x76e290, 0x5a3920, 0x5a3960,
    { 0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x01, 0xda,
      0xa5, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    0x5a74b0,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0xf8, 0xa8, 0x65, 0x01 },
    BuildMultilingual },
};

// Resolved once by initializePhyreFixes, and shared by every fix in this module
// so none of them repeats the fingerprint check.
BYTE* g_base = nullptr;
const PhyreGame* g_game = nullptr;

const PhyreGame* recognizeExecutable(BYTE*& baseOut) {
  ModuleIdentity id;
  if (!currentModuleIdentity(id))
    return nullptr;
  for (const PhyreGame& game : kGames) {
    if (_stricmp(id.name, game.executable) || id.textSize != game.textSize)
      continue;
    baseOut = id.base;
    return &game;
  }
  log("phyre: unrecognized executable ", id.name,
      " .text=", reinterpret_cast<void*>(uintptr_t(id.textSize)));
  return nullptr;
}

}  // namespace

namespace dusk {

bool initializePhyreFixes() {
  static const bool initialized = [] {
    // Identity is resolved independently of the address-based feature set.
    // Shared D3D fixes (SMAA, sharpening and supersampling) still
    // need a verified host when every Phyre address fix is disabled.
    g_game = recognizeExecutable(g_base);
    if (!g_game)
      return false;

    // This registers a predicate with core's scene-pass module. It carries no
    // address and installs no hook, but it is still registered only after exact
    // recognition so an unknown same-prefix executable receives no D3D policy.
    registerPhyreSceneTarget();

    const bool wantCache = atfix::featureEnabled(atfix::Feature::AtlasCache);
    const bool wantVerify = atfix::featureEnabled(atfix::Feature::AtlasVerify);
    const bool wantWorldMap =
      atfix::featureEnabled(atfix::Feature::WorldMapCursor);
    const bool wantField =
      atfix::featureEnabled(atfix::Feature::FieldEngineFix) ||
      atfix::featureEnabled(atfix::Feature::FieldStabilizer);
    const bool wantLogoSkip =
      atfix::featureEnabled(atfix::Feature::SkipStartupLogos);
    const bool wantMovieSkip =
      atfix::featureEnabled(atfix::Feature::SkipIntroMovie);
    const bool wantAddressFix =
      wantCache || wantVerify || wantField || wantWorldMap || wantLogoSkip ||
      wantMovieSkip;
    if (wantAddressFix) {
      // ALREADY_INITIALIZED is a success here. DllMain initializes MinHook for
      // the window-background fix, which has to be in place before the game
      // registers its window class, so by the time this runs MinHook is normally
      // already up. Treating that as a failure would decline every Ayesha fix.
      const MH_STATUS init = MH_Initialize();
      if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log("phyre: MH_Initialize failed (", MH_StatusToString(init), ")");
        return false;
      }
    }

    if (wantCache || wantVerify)
      installAtlasFix(g_base, *g_game, wantCache, wantVerify);

    // Independent of the atlas hooks: the field fix has its own addresses and
    // its own prologue checks, so it installs (or declines) on its own terms.
    if (wantField)
      atfix::installFieldPhysics(g_base, g_game->exeBuild);

    // Same family as the field fix above: another movement value applied per
    // frame instead of per second, in a different subsystem.
    if (wantWorldMap)
      installWorldMapFix(g_base, g_game->exeBuild);

    // The two startup skips. Each carries its own addresses and its own
    // prologue windows, so each installs or declines on its own terms.
    if (wantLogoSkip)
      atfix::installLogoSkip(g_base, g_game->exeBuild);
    if (wantMovieSkip)
      atfix::installMovieSkip(g_base, g_game->exeBuild);

    // Not a Phyre fix at all: the pad layer is Gust framework code shared by all
    // six DX ports, so core owns the mechanism and this module only supplies the
    // row. It gates itself on its own feature, which is why there is no `want`
    // flag here.
    atfix::installPadRescanBackoff(g_base,
      { g_game->padCreateWrapperRva, g_game->padCreateExpected });

    // Observes the rescan's other half and installs nothing: it asks whether a
    // controller arriving under Proton is announced to this process, which is
    // what would let the rescan be suppressed outright instead of rate-limited.
    // Off unless DUSK_PAD_NOTIFY_TRACE is set. Here rather than in DllMain
    // because it starts a thread, which the loader lock forbids.
    atfix::startPadNotifyTrace();

    // Recognition, not whether an address-based option happened to be enabled,
    // is the contract with core: true authorizes the shared D3D fix surface.
    return true;
  }();
  return initialized;
}

void phyreFrameTick() {
  atlasFixFrameTick();
}

}  // namespace dusk
