// SPDX-License-Identifier: MIT
//
// PhyreEngine module entry point: executable recognition, the Ayesha address
// pack, and the install order for the fixes in this directory. The fixes
// themselves live alongside -- atlas_fix.cpp, field_physics.cpp -- and none of
// them recognizes the executable itself; they are handed the base and the
// verified descriptor from here.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "phyre.h"

#include "atlas_fix.h"
#include "field_physics.h"
#include "logo_skip.h"
#include "movie_skip.h"
#include "shadow_tap.h"
#include "field_collision_fix.h"
#include "worker_idle_sleep.h"
#include "scene_target.h"
#include "worldmap_fix.h"
#include "../../core/game.h"
#include "../../core/mix_card.h"
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
//
// mixCardUpdateRva was derived by searching each build for the pump's own idiom,
// `cvttss2si ebx, xmm1` followed by the five-byte alignment NOP that stands where
// its pre-test belongs: `find-bytes <exe> f30f2cd90f1f440000` returns exactly one
// occurrence in each, inside a 0x83-byte function whose accumulator is
// `[rcx+0x820]` and whose two constants are the 1/59.94 and 59.94 bit patterns.
// See core/mix_card.h for what the function does and what the correction is.
constexpr PhyreGame kGames[] = {
  { "Atelier_Ayesha_EN.exe", 0x984df4,
    0x74bd90, 0x581420, 0x581460,
    { 0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x01, 0xff,
      0xa7, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    0x584fb0,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0x80, 0x51, 0x4e, 0x01 },
    0x3a8b50,
    0x28a6c1,
    BuildEnglish },
  { "Atelier_Ayesha.exe", 0x9a9604,
    0x76e290, 0x5a3920, 0x5a3960,
    { 0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x01, 0xda,
      0xa5, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    0x5a74b0,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0xf8, 0xa8, 0x65, 0x01 },
    0x3bd820,
    0x2926f1,
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
      atfix::featureEnabled(atfix::Feature::FieldEngineFix);
    const bool wantLogoSkip =
      atfix::featureEnabled(atfix::Feature::SkipStartupLogos);
    const bool wantMovieSkip =
      atfix::featureEnabled(atfix::Feature::SkipIntroMovie);
    const bool wantSynthRate =
      atfix::featureEnabled(atfix::Feature::SynthesisAnimationRate);
    const bool wantAddressFix =
      wantCache || wantVerify || wantField || wantWorldMap || wantLogoSkip ||
      wantMovieSkip || wantSynthRate;
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

    // The other half of the shadow-map enlargement, which core allocates and
    // redirects but cannot finish: the filter width is stated in the engine's
    // own code, so correcting it needs this module's addresses. It gates itself
    // on the multiplier, which is why there is no `want` flag here.
    atfix::installShadowTapScale(g_base, g_game->exeBuild);

    // Clamps the character-separation depth at zero, so bodies that are
    // already apart stop being pulled together. DUSK_COLLIDE_GAIN turns the
    // same patch site into the diagnostic that demonstrates the defect.
    atfix::installFieldCollision(g_base, g_game->exeBuild);

    // Shortens the idle poll of the worker a scene transition joins, which is
    // most of what a transition costs. Gates itself on the bytes at its row's
    // address and on its own environment switch.
    atfix::installWorkerIdleSleep(g_base, g_game->workerIdleSleepRva);

    // Not a Phyre fix at all: the pad layer is Gust framework code shared by all
    // six DX ports, so core owns the mechanism and this module only supplies the
    // row. It gates itself on its own feature, which is why there is no `want`
    // flag here.
    atfix::installPadRescanBackoff(g_base,
      { g_game->padCreateWrapperRva, g_game->padCreateExpected });

    // Also not a Phyre fix: `Card` is Gust framework code rather than either
    // engine's, so the same bottom-tested pump ships in all six games and core
    // owns the correction. Its prologue is byte-identical in all twelve builds,
    // so unlike the pad wrapper above this window is shared rather than per-row.
    // See core/mix_card.h. It gates itself on its own feature.
    atfix::installMixCardFix(g_base,
      { g_game->mixCardUpdateRva,
        { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
          0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0xf3 } });

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
