// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Which colour+depth bind is the 3D scene on Escha & Logy and Shallie.
//
// WHY NOT THE BLOOM COMPOSITE. The natural anchor is hanging pre-UI
// antialiasing on the
// bloom composite in `PostEffectGlow.kps`, identified by checksum. That anchor
// works -- it is found in the process and binds once per frame, every frame, in
// gameplay -- but a dump of its own render target taken immediately after its
// draw shows the entire interface already present: the date panel,
// the area scroll, the button prompts. This engine draws its interface INTO the
// scene and then runs the post chain over everything, so the composite is
// post-UI and antialiasing there would soften the interface exactly like the
// Present-time pass that is already shipped and already opt-in for that reason.
//
// The pre-UI moment is therefore much earlier than anyone was looking: the
// transition out of scene rendering, before the first interface draw and before
// any post-effect. Antialias there and the order becomes scene, SMAA, interface
// on top, bloom over the lot. That is what this test is for.
//
// THE RULE, and what is measured versus what is inherited:
//
//   colour  BindFlags exactly RENDER_TARGET|SHADER_RESOURCE (0x28)
//   depth   carries DEPTH_STENCIL
//   both    the same size, and at least the swap chain's
//
// The colour predicate comes from a static read of the two creation paths: one
// sets `or eax, 8` and yields 0x28/0x48, the other does not and yields
// 0x20/0x40. It is what separates the scene colour from the post chain's own
// targets, which are 0x20.
//
// THE DEPTH PREDICATE IS DELIBERATELY LOOSER THAN THE STATIC READ. Reading the
// same two creation paths derives `0x48` exactly -- depth allocated readable. A
// target census run measured the only screen-sized depth at
// `0x40`, no SHADER_RESOURCE. Those disagree, and the census is the weaker
// evidence of the two because it only ever ran over the title screen, which has
// no 3D scene at all. Requiring 0x48 on the strength of a static read that the
// one available measurement contradicts is how a test declines every bind for a
// session and reports nothing but zeroes. So this requires DEPTH_STENCIL and
// logs what it actually accepts, which settles the disagreement in one run.
//
// SIZE IS A FLOOR, NOT AN EQUALITY, and that is not laziness. These games
// allocate scene targets at the RENDER size, which is the ini resolution rather
// than the swap chain's -- and under the present clamp those two deliberately
// disagree, the render size being the larger. An equality against the swap chain
// would be exactly false for the scene pair and exactly true for the back
// buffer, which is the inversion this test exists to avoid. A floor is true in both
// configurations and still excludes the 1024x1024 shadow pair.
namespace atfix {

void installKtglSceneTarget();

// The pre-UI anchor, found by mapping a frame. Its target/run/armed tuple lives
// on the recording context as retained D3D private data, so replacement
// deferred contexts neither overwrite nor inherit one another. See
// scene_target.cpp.
void ktglPreUiNoteTargets(ID3D11DeviceContext* context, unsigned int numViews,
                          struct ID3D11RenderTargetView* const* views);
// Returns an owned texture reference; the caller must Release it.
struct ID3D11Texture2D* ktglPreUiNoteDraw(ID3D11DeviceContext* context);

// The anchor itself, called after a forwarded draw. Exposed because the raster
// correction owns the draw slots whenever supersampling is on, and it reaches
// this through ScenePolicy::afterDraw.
void ktglPreUiAfterDraw(ID3D11DeviceContext* context);
void ktglPreUiFrameTick();

// Wiring for d3d11_hooks.cpp: the four draw slots this feature owns.
void STDMETHODCALLTYPE hookedPreUiDraw(ID3D11DeviceContext*, UINT, UINT);
void STDMETHODCALLTYPE hookedPreUiDrawIndexed(ID3D11DeviceContext*, UINT, UINT,
                                              INT);
void STDMETHODCALLTYPE hookedPreUiDrawIndexedInstanced(ID3D11DeviceContext*,
                                                       UINT, UINT, UINT, INT,
                                                       UINT);
void STDMETHODCALLTYPE hookedPreUiDrawInstanced(ID3D11DeviceContext*, UINT,
                                                UINT, UINT, UINT);

}  // namespace atfix
