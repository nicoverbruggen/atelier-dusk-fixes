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
#include "scene_target.h"
#include "worldmap_fix.h"
#include "../../core/game.h"
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

// The two Ayesha builds. SHA-256 and .text size for each are in WORK_DOC.md
// 1.7, together with how every RVA was derived.
//
// The unlock prologue is per-row because the hooked unlock is a jmp stub and its
// 16-byte window carries the rel32 displacement (WORK_DOC.md "The unlock has
// two levels; the stub is hooked"). The other three windows are
// build-independent and live in atlas_fix.cpp with the hooks that check them.
constexpr PhyreGame kGames[] = {
  { "Atelier_Ayesha_EN.exe", 0x984df4,
    0x078320, 0x74bd90, 0x581420, 0x581460,
    { 0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x01, 0xff,
      0xa7, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    0x584fb0,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
      0xd9, 0x48, 0x8b, 0x0d, 0x80, 0x51, 0x4e, 0x01 },
    BuildEnglish },
  { "Atelier_Ayesha.exe", 0x9a9604,
    0x07a8d0, 0x76e290, 0x5a3920, 0x5a3960,
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
    // Before the feature gate below, and deliberately so. This registers a
    // predicate with core's MSAA module -- no hooks, no mapped addresses, no
    // fingerprint needed -- and a session that enables only MSAA must still get
    // it. Everything past the gate is a hooked, address-dependent fix; this is
    // not one, and gating it with them would make MSAA silently decline every
    // bind in exactly the configuration that asked for it.
    registerPhyreSceneTarget();

    const bool wantCache = atfix::featureEnabled(atfix::Feature::AtlasCache);
    const bool wantStats = atfix::featureEnabled(atfix::Feature::AtlasStats);
    const bool wantTrace = atfix::featureEnabled(atfix::Feature::AtlasTrace);
    const bool wantVerify = atfix::featureEnabled(atfix::Feature::AtlasVerify);
    const bool wantCensus = atfix::featureEnabled(atfix::Feature::AtlasCensus);
    const bool wantWorldMap =
      atfix::featureEnabled(atfix::Feature::WorldMapCursor);
    const bool wantField =
      atfix::featureEnabled(atfix::Feature::FieldEngineFix) ||
      atfix::featureEnabled(atfix::Feature::FieldStabilizer) ||
      atfix::fieldTraceEnabled();
    if (!wantCache && !wantStats && !wantTrace && !wantVerify && !wantCensus &&
        !wantField && !wantWorldMap)
      return false;
    if (MH_Initialize() != MH_OK) {
      log("phyre: MH_Initialize failed");
      return false;
    }

    // One recognition for every Ayesha-specific fix in this DLL.
    g_game = recognizeExecutable(g_base);
    if (!g_game)
      return false;

    if (wantCache || wantStats || wantTrace || wantVerify || wantCensus)
      installAtlasFix(g_base, *g_game, wantCache, wantStats, wantTrace,
                      wantVerify, wantCensus);

    // Independent of the atlas hooks: the field fix has its own addresses and
    // its own prologue checks, so it installs (or declines) on its own terms.
    if (wantField)
      atfix::installFieldPhysics(g_base, g_game->exeBuild);

    // Same family as the field fix above: another movement value applied per
    // frame instead of per second, in a different subsystem.
    if (wantWorldMap)
      installWorldMapFix(g_base, g_game->exeBuild);

    // Not a Phyre fix at all: the pad layer is Gust framework code shared by all
    // six DX ports, so core owns the mechanism and this module only supplies the
    // row. It gates itself on its own feature, which is why there is no `want`
    // flag here.
    atfix::installPadRescanBackoff(g_base,
      { g_game->padCreateWrapperRva, g_game->padCreateExpected });

    return true;
  }();
  return initialized;
}

void phyreFrameTick() {
  atlasFixFrameTick();
}

}  // namespace dusk
