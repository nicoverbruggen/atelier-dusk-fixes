// SPDX-License-Identifier: MIT
//
// The empty policy. See scene_policy.h.
#include "scene_policy.h"

namespace atfix {
namespace {

bool sceneNo() { return false; }
void sceneNoTargets(ID3D11DeviceContext*, unsigned int,
                    ID3D11RenderTargetView* const*) {}
void sceneNoAfterDraw(ID3D11DeviceContext*) {}
void sceneNoTick() {}

}  // namespace

const ScenePolicy& sceneNoPolicy() {
  static const ScenePolicy policy = {
    sceneNo,           // preUiAtFirstDraw
    sceneNoTargets,
    sceneNoTick,
    sceneNoAfterDraw,
    sceneNo,           // needsMainRenderSize
    { nullptr, nullptr, nullptr, nullptr },
  };
  return policy;
}

}  // namespace atfix
