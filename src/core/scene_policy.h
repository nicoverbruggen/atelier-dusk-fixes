// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// The questions the pre-UI pass asks that the two engines answer differently.
//
// WHY THIS EXISTS. "Antialias and sharpen the scene before the interface is
// drawn into it" is one feature, and the moment it names is a different moment
// on each engine. Ayesha reaches it through the render-target transition that
// scene_pass.cpp observes; Escha & Logy and Shallie reach it through the first
// draw into the surface the interface is about to be drawn into, which their
// own module identifies. Neither moment means anything on the other engine.
//
// Before this header, core asked KTGL. `scene_pass.cpp` gated Ayesha's pass on
// `!ktglPreUiActive()`, `main.cpp` called `ktglPreUiFrameTick()` in every
// process, and `d3d11_hooks.cpp` decided whether to install the draw detours
// with `highRes.rasterCorrection || ssaaConfigured()` -- a PhyreEngine
// condition -- and then logged the decision as a KTGL one. On Ayesha that line
// printed "supersampling is on" in a session where supersampling was off.
//
// PULLED, NOT PUSHED, for the same reason as SsaaPolicy: the first caller is a
// D3D11 hook that can run before any engine module has initialized, so a policy
// that had to be registered at init would arrive after the call that needed it.
// currentEngine() fingerprints the running executable and needs no
// initialization. See supersample_policy.h, which states the argument in full.
namespace atfix {

struct ScenePolicy {
  // Does this engine identify the pre-UI moment from the draw stream instead?
  // When true, d3d11_hooks.cpp installs the draw detours for it.
  bool (*preUiAtFirstDraw)();

  // The draw-stream anchor, when there is one. noteTargets watches the binds
  // and arms. Never null: an engine without that anchor supplies the empty
  // callback, so no caller has to test first.
  void (*noteTargets)(ID3D11DeviceContext* context, unsigned int numViews,
                      ID3D11RenderTargetView* const* views);
  void (*frameTick)();

  // Called from WHICHEVER draw detour is installed, after the draw is
  // forwarded. Two families can own those four vtable slots -- the raster
  // correction's and the pre-UI set's -- and only one of them is installed in
  // any session, so an anchor that fires from draws has to be reachable from
  // both. On Ayesha the raster correction always wins the slots, which is why
  // its pre-UI anchor could not simply have its own detours.
  //
  // Empty on an engine whose anchor does not fire from draws.
  void (*afterDraw)(ID3D11DeviceContext* context);

  // Does this engine's scene test depend on the high-resolution fix having
  // learned a main render size? Phyre's does and says so; KTGL's is structural
  // and does not. This drives one warning, which was printed at every KTGL
  // session of a working feature before the question was asked per engine.
  bool (*needsMainRenderSize)();

  // The four draw detours the first-draw anchor fires from, in vtable-slot
  // order: DrawIndexed(12), Draw(13), DrawIndexedInstanced(20),
  // DrawInstanced(21). The detours are engine code -- they know which surface
  // the anchor produced and what to run on it -- so the engine supplies them
  // and d3d11_hooks.cpp only installs them. All four are null on an engine
  // without a draw anchor, and d3d11_hooks.cpp installs nothing then.
  //
  // Typed as void* because ContextHookSpec is private to d3d11_hooks.cpp, and
  // handing every engine module the hook table's layout would undo the point of
  // that file owning the vtable alone.
  void* drawDetours[4];
};

// This process's policy, resolved once from the running executable.
const ScenePolicy& scenePolicy();

// Every answer off, and the two hooks empty. Returned for an executable that is
// neither engine, and used by each engine policy for the half it does not have.
const ScenePolicy& sceneNoPolicy();

}  // namespace atfix
