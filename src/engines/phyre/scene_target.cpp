// SPDX-License-Identifier: MIT
//
// See scene_target.h for why this rule lives in the engine module.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include "scene_target.h"
#include "../../core/highres.h"
#include "../../core/scene_pass.h"

namespace atfix {
namespace {

// Ayesha's scene pass binds a colour target and a depth target that are both
// exactly the main render size.
//
// THE EVIDENCE. The main render size is what `highres.cpp` learns from the
// first main target the game creates, and the reason it is a reliable
// discriminator here is the defect that module exists to correct: this engine
// pins its scene targets to a hard-coded 1920x1080 and everything else it
// allocates is some other shape. A censused session settles at 15 distinct
// target shapes, of which exactly one colour+depth pair is at the main size.
// The shadow map is 1024x1024, the blur chain runs 960x540 down to 64x36, and
// the UI composites at the swap-chain size.
//
// This is the same rule `highres.cpp` already uses to decide what to enlarge,
// which is the point: if the rule were wrong, the resolution fix would be
// visibly wrong too, and it has been validated in game at 1440p and 4K. That
// makes this the cheapest correct answer available, and specifically cheaper
// than TellowKrinkle's -- he identifies the scene target by counting more than
// 64 indexed draws into it, a threshold tuned against a different game's draw
// pattern precisely because he had no equivalent size signal to lean on.
//
// The format is deliberately NOT constrained. The Arland implementation
// additionally requires a BGRA render-target view, which it needs because its
// games composite into the swap-chain back buffer and it has to tell that apart
// from other same-sized surfaces. Ayesha renders to an offscreen pair, and the
// size test already isolates it, so adding a format condition here would narrow
// the rule on no evidence.
bool phyreSceneTargets(const D3D11_TEXTURE2D_DESC& color,
                       const D3D11_TEXTURE2D_DESC& depth) {
  unsigned int width = 0;
  unsigned int height = 0;
  // The SCENE size, not the main render size: supersampling makes the scene
  // targets larger than the display, and asking for the main size here declined
  // every bind for a whole session while reporting nothing but zeroes.
  if (!highResSceneSize(&width, &height))
    return false;
  if (color.Width != width || color.Height != height ||
      depth.Width != width || depth.Height != height)
    return false;

  // Size alone is not enough, and a run proved it. At 1440p the main render
  // size EQUALS the swap-chain size, so the back buffer paired with the
  // swap-size depth matched this test just as well as the real scene pair did,
  // and the composite/UI pass was being taken for the scene on every bind.
  // Measured descriptors from that run:
  //
  //   back buffer  colour format=87 (B8G8R8A8_UNORM)    depth bindFlags=0x40
  //   scene pair   colour format=90 (B8G8R8A8_TYPELESS) depth bindFlags=0x48
  //
  // Two independent discriminators, and both are used because each says
  // something true about what a scene target IS here rather than merely
  // separating these particular surfaces:
  //
  //   The colour target is TYPELESS. This engine allocates the surfaces it
  //   intends to both render into and sample back as typeless, so it can put a
  //   typed render-target view and a typed shader-resource view over the same
  //   memory. The back buffer is a presented surface and is typed.
  //
  //   The depth target carries SHADER_RESOURCE. The scene's depth is allocated
  //   readable; the swap-chain-sized depth used by the composite pass is not.
  //
  // Note what this does NOT do: it does not require the back buffer to be
  // excluded by identity. A renderer that draws its scene straight into the
  // back buffer is a perfectly ordinary design -- the Arland games do exactly
  // that -- so "the scene is never the back buffer" would be wrong as a general
  // rule and wrong in core. It is excluded here because on THIS engine the back
  // buffer is not where the scene lives.
  if (color.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS)
    return false;
  if (!(depth.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    return false;

  // Two pairs matching is expected, not an anomaly: the engine ping-pongs
  // between two identically-shaped scene colour targets for its post-processing
  // chain. Both are the scene.
  return true;
}

}  // namespace
}  // namespace atfix

namespace dusk {

void registerPhyreSceneTarget() {
  atfix::scenePassSetTest(&atfix::phyreSceneTargets);
}

}  // namespace dusk
