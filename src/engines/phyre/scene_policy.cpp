// SPDX-License-Identifier: MIT
//
// Ayesha's answers. See core/scene_policy.h.
#include "scene_policy.h"

#include "../../core/scene_policy.h"
#include "pre_ui.h"

namespace atfix {
namespace {

// The anchor fires from the draw stream, but it does NOT ask for the pre-UI
// detour set: the raster correction already owns those four slots in every
// Ayesha session, and it calls afterDraw for us. Answering true here would ask
// d3d11_hooks.cpp for a second set on slots it cannot have.
bool phyreAtFirstDraw() { return false; }

// The scene test is `phyreSceneTargets`, which compares against the size the
// high-resolution fix learned. With that fix off it can never match, and the
// warning in d3d11_hooks.cpp is the only notice anyone gets.
bool phyreNeedsMainRenderSize() { return true; }

}  // namespace

const ScenePolicy& phyreScenePolicy() {
  static const ScenePolicy policy = {
    phyreAtFirstDraw,
    phyrePreUiNoteTargets,
    sceneNoPolicy().noteDraw,      // the anchor fires from afterDraw, not here
    phyrePreUiFrameTick,
    phyrePreUiAfterDraw,
    phyreNeedsMainRenderSize,
    { nullptr, nullptr, nullptr, nullptr },   // no draw anchor on this route
  };
  return policy;
}

}  // namespace atfix
