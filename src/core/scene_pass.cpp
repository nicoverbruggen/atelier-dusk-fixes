// SPDX-License-Identifier: MIT
//
// See scene_pass.h for what this module answers and why the verdict itself
// lives in an engine module rather than here.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdlib>

#include "log.h"
#include "d3d11_hooks.h"
#include "frame_map.h"
#include "../engines/ktgl/scene_target.h"
#include "scene_pass.h"
#include "sharpen.h"
#include "smaa.h"
#include "supersample.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {


// HOW MANY DISTINCT SURFACES THE TEST ACCEPTS, which is the question the leave
// count just redirected us to. One leave per frame means the trigger fires at
// the end of scene rendering, so timing is not the fault -- but if several
// targets satisfy the test, the pass antialiases one and the composite reads
// another, and the result is exactly what was seen: the debug edge map showing
// through in some regions and only some ground textures sharpened.
//
// The private record warns that Escha has a ping-pong pool of screen-sized
// 0x28 colour targets that Shallie lacks. This counts them rather than assuming
// the warning applies.
constexpr int kMaxAccepted = 16;
void* g_accepted[kMaxAccepted] = {};
std::atomic<int> g_acceptedCount{0};

void noteAccepted(ID3D11Texture2D* colour) {
  const int n = g_acceptedCount.load(std::memory_order_relaxed);
  for (int i = 0; i < n && i < kMaxAccepted; ++i)
    if (g_accepted[i] == colour)
      return;
  if (n >= kMaxAccepted)
    return;
  g_accepted[n] = colour;
  g_acceptedCount.store(n + 1, std::memory_order_relaxed);
}

// The scene colour last bound on this context. Per-context, because Ayesha
// records its scene on a deferred context and replays it on the immediate one,
// so a single global would mix the two.
const GUID IID_DuskSceneColor =
  { 0x7c1f4a20, 0x5d63, 0x4b8e, { 0x9a, 0x14, 0x2e, 0x77, 0x0b, 0x35, 0xc1, 0x06 } };

SceneTargetTest g_sceneTest = nullptr;

}  // namespace

void scenePassSetTest(SceneTargetTest test) {
  g_sceneTest = test;
}

void scenePassNoteBoundary(ID3D11DeviceContext* context, unsigned int numViews,
                           ID3D11RenderTargetView* const* views,
                           ID3D11DepthStencilView* depth) {
  const SceneTargetTest sceneTest = g_sceneTest;
  if (!context || (!smaaPreUiEnabled() && !ssaaConfigured()))
    return;
  if (!sceneTest) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
      log("SCENEPASS: no scene-target test is registered for this engine, so"
          " the scene is never identified. The pre-UI SMAA pass and"
          " supersampling both do nothing this session.");
    return;
  }

  // The arriving colour target, and whether it is the scene. Both are needed:
  // the scene test decides whether this is still the scene pass, and the target
  // itself is what SMAA will run on once the composite has drawn into it.
  ID3D11Texture2D* arrivingColor = nullptr;
  bool arrivingIsScene = false;
  if (numViews == 1 && views && views[0]) {
    ID3D11Resource* colorResource = nullptr;
    views[0]->GetResource(&colorResource);
    if (colorResource) {
      ID3D11Texture2D* colorTex = nullptr;
      if (SUCCEEDED(colorResource->QueryInterface(IID_ID3D11Texture2D,
            reinterpret_cast<void**>(&colorTex))) && colorTex) {
        arrivingColor = colorTex;   // ownership moves here
        if (depth) {
          ID3D11Resource* depthResource = nullptr;
          depth->GetResource(&depthResource);
          ID3D11Texture2D* depthTex = nullptr;
          if (depthResource &&
              SUCCEEDED(depthResource->QueryInterface(IID_ID3D11Texture2D,
                reinterpret_cast<void**>(&depthTex))) && depthTex) {
            D3D11_TEXTURE2D_DESC cd = {};
            D3D11_TEXTURE2D_DESC dd = {};
            arrivingColor->GetDesc(&cd);
            depthTex->GetDesc(&dd);
            arrivingIsScene = sceneTest(cd, dd);
            depthTex->Release();
          }
          if (depthResource) depthResource->Release();
        }
      }
      colorResource->Release();
    }
  }

  ID3D11Texture2D* previous = nullptr;
  UINT size = sizeof(previous);
  if (FAILED(context->GetPrivateData(IID_DuskSceneColor, &size, &previous)))
    previous = nullptr;

  // Scene -> not-scene: the scene is complete and the interface is not yet
  // drawn, so `previous` is the surface to antialias.
  //
  // NOTE, and it is the open question of this whole area: this transition
  // happens 5-22 times per frame, because the engine steps in and out of its
  // scene targets while running its post-processing chain. Only the last one is
  // the composite. SMAA tolerates that because its own once-per-frame latch
  // makes the FIRST one win and the scene is largely complete by then -- but
  // "largely" is doing work in that sentence and it has never been verified
  // against a census. Anything with stricter timing needs the composite
  // identified positively, not inferred from this transition -- which is
  // exactly what supersampling now does, and why it fires nothing here.
  //
  // So when supersampling has engaged this pass stands down entirely and SMAA
  // runs inside the downscale instead, on the display-sized result. Running
  // both would antialias the scene twice; running this one would additionally
  // do it at the supersampled size, where a morphological filter's pixel-counted
  // search distances are wrong by the supersampling factor.
  //
  // Keyed on ENGAGED rather than on configured. If supersampling is configured
  // and never attaches -- the composite is never identified, the pass fails, a
  // second host is refused -- then keying on configuration alone would leave
  // SMAA with nowhere to run, and it would fall back to the Present path and
  // antialias the interface. That is strictly worse than turning supersampling
  // off. The frame latch inside smaaApplySceneColor makes the brief overlap
  // harmless: for the first frames before SSAA engages, SMAA runs here at scene
  // resolution; afterwards the in-pass call at display resolution claims the
  // frame first.
  // A NOTE THE COMMENT ABOVE EARNED. It has warned since it was written that
  // "the scene is largely complete by the first transition" had never been
  // checked. It was checked on 2026-08-10 by counting: this engine leaves the
  // scene target exactly once per frame, so the concern does not apply here --
  // and the reason the transition is still the wrong place on KTGL is that the
  // interface is drawn afterwards, into a different surface. See
  // engines/ktgl/scene_target.cpp.

  // The Glow-anchored variant claims the frame itself, and the latch inside
  // smaaApplySceneColor means whichever fires first wins -- so this one has to
  // stand down entirely when that is on, or it would keep antialiasing the
  // surface nothing reads.
  static const bool atGlow = [] {
    const char* env = std::getenv("DUSK_SMAA_AT_GLOW");
    return env && env[0] != '0';
  }();
  // Nor when this engine has a pre-UI anchor of its own. That anchor fires
  // later in the frame, on the surface the interface is about to be drawn into,
  // and this call would claim the frame's one SMAA pass before it got there --
  // which is exactly what happened: the anchor was called every frame and
  // refused every frame, and the only symptom was that nothing changed.
  if (previous && !arrivingIsScene && !ssaaEngaged() && !atGlow &&
      !ktglPreUiActive())
    // Sharpening runs on the same surface immediately after, while the
    // interface is still not in it -- the same order the KTGL anchor uses, so
    // the setting means one thing across all three games.
  {
    smaaApplySceneColor(context, previous);
    // Independent of the smoothing above: with edge smoothing off this is a
    // sharpening filter on the finished scene, still before the interface.
    sharpenApply(context, previous);
  }

  if (arrivingIsScene) {
    noteAccepted(arrivingColor);
    context->SetPrivateDataInterface(IID_DuskSceneColor, arrivingColor);
    // Tag it for supersampling. This module owns the verdict "that surface is
    // the scene"; supersample.cpp owns storing it, so there is still exactly
    // one place that decides. No-op when supersampling is off.
    ssaaTagSceneHost(arrivingColor);
  } else if (previous) {
    context->SetPrivateData(IID_DuskSceneColor, 0, nullptr);
  }

  if (arrivingColor) arrivingColor->Release();

  if (previous) previous->Release();
}

// ---- the interception points ----------------------------------------------
//
// Each of these forwards unconditionally and asks this module what else to do.
// None changes behaviour when both consumers are off: scenePassNoteBoundary
// returns immediately in that case, so an ordinary session pays a
// predictable-branch call per intercepted method and nothing else.

// The boundary is observed before the composite marker is set, and both run
// before the bind is forwarded. The order of those two is a correctness
// requirement rather than a style: scenePassNoteBoundary is what tags the
// arriving surface as a scene colour host, and ssaaNoteTargetsBound decides
// whether the bind now arriving is the composite that will sample it.
void STDMETHODCALLTYPE hookedOMSetRenderTargets(
    ID3D11DeviceContext* self, UINT numViews,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) {
  frameMapNoteTargets(self, numViews, views);
  ktglPreUiNoteTargets(numViews, views);
  scenePassNoteBoundary(self, numViews, views, depth);

  // Is the swap chain's back buffer among the targets arriving? On this engine
  // that is the composite and nothing else, and it is the positive
  // identification the whole supersampling rebuild rests on.
  ssaaNoteTargetsBound(self, numViews, views);

  d3d11OriginalsFor(self).omSetRenderTargets(self, numViews, views, depth);
}

void STDMETHODCALLTYPE hookedPSSetShaderResources(
    ID3D11DeviceContext* self, UINT startSlot, UINT numViews,
    ID3D11ShaderResourceView* const* views) {
  // The composite's sample IS the resample, so replacing what it samples
  // replaces the filter -- a box filter sized to the ratio instead of the
  // engine's four bilinear taps. The substitution lives in this array and
  // nowhere else: it is gone the moment this call returns, so the
  // post-processing passes that sample the same texture cannot inherit it.
  ID3D11ShaderResourceView* substituted[kSsaaMaxSubstitutedViews];
  if (ssaaSubstituteShaderResources(self, startSlot, numViews, views,
                                    substituted, kSsaaMaxSubstitutedViews)) {
    d3d11OriginalsFor(self).psSetShaderResources(self, startSlot, numViews,
                                                 substituted);
    return;
  }

  d3d11OriginalsFor(self).psSetShaderResources(self, startSlot, numViews, views);
}

HRESULT STDMETHODCALLTYPE hookedFinishCommandList(
    ID3D11DeviceContext* self, BOOL restoreState,
    ID3D11CommandList** commandList) {
  // The composite marker is per-context state, and a list can be closed with
  // the back buffer still bound. Dropping it here keeps the next recording on
  // this context from starting out believing it is inside the composite.
  ssaaClearContextState(self);
  return d3d11OriginalsFor(self).finishCommandList(self, restoreState,
                                                   commandList);
}

void STDMETHODCALLTYPE hookedOMSetRenderTargetsAndUnorderedAccessViews(
    ID3D11DeviceContext* self, UINT numViews,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth,
    UINT uavStart, UINT numUavs, ID3D11UnorderedAccessView* const* uavs,
    const UINT* uavInitialCounts) {
  // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL means "leave the render targets
  // exactly as they are and only touch the UAVs". No boundary is crossed and
  // the composite marker must be left exactly as it is -- treating the sentinel
  // as a count would be a wild read, and clearing the marker on it would drop
  // the composite half way through.
  if (numViews != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
    frameMapNoteTargets(self, numViews, views);
  scenePassNoteBoundary(self, numViews, views, depth);
    ssaaNoteTargetsBound(self, numViews, views);
  }

  d3d11OriginalsFor(self).omSetRenderTargetsAndUnorderedAccessViews(
    self, numViews, views, depth, uavStart, numUavs, uavs, uavInitialCounts);
}

}  // namespace atfix

namespace atfix {

// Called from the hooked Present. Reports how many times a frame leaves the
// scene target, which decides whether "fire on the last one" is implementable.
// The surface the test accepted, for whoever needs to check that it is the same
// one the engine reads downstream. Raw and compared only, never dereferenced.
void* scenePassAcceptedSurface() {
  return g_acceptedCount.load(std::memory_order_relaxed) ? g_accepted[0]
                                                         : nullptr;
}

void scenePassFrameTick() {
  // Nothing per-frame to reset here any more. Kept as the hook point because
  // Present is the right place for it and the next thing that needs one will
  // want it.
}

}  // namespace atfix
