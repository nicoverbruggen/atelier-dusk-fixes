// SPDX-License-Identifier: MIT
//
// Escha & Logy and Shallie's answers. See core/scene_policy.h.
#include "scene_policy.h"

#include "../../core/scene_policy.h"
#include "scene_target.h"

namespace atfix {
namespace {

bool ktglAtFirstDraw() { return true; }

// Structural: the anchor identifies the moment from the shape of the draw
// stream and never asks what size the game renders at.
bool ktglNeedsMainRenderSize() { return false; }

void ktglNoteTargets(ID3D11DeviceContext*, unsigned int numViews,
                     ID3D11RenderTargetView* const* views) {
  ktglPreUiNoteTargets(numViews, views);
}

ID3D11Texture2D* ktglNoteDraw() { return ktglPreUiNoteDraw(); }

void ktglTick() { ktglPreUiFrameTick(); }

}  // namespace

const ScenePolicy& ktglScenePolicy() {
  static const ScenePolicy policy = {
    ktglAtFirstDraw,
    ktglNoteTargets,
    ktglNoteDraw,
    ktglTick,
    // Its own detours call this directly; this pointer is how the RASTER
    // correction's detours reach it, which is the only way the pass runs at all
    // when supersampling is on.
    ktglPreUiAfterDraw,
    ktglNeedsMainRenderSize,
    { reinterpret_cast<void*>(&hookedPreUiDrawIndexed),
      reinterpret_cast<void*>(&hookedPreUiDraw),
      reinterpret_cast<void*>(&hookedPreUiDrawIndexedInstanced),
      reinterpret_cast<void*>(&hookedPreUiDrawInstanced) },
  };
  return policy;
}

}  // namespace atfix
