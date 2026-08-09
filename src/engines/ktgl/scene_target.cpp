// SPDX-License-Identifier: MIT
//
// Which colour+depth bind is the 3D scene on Escha & Logy and Shallie.
//
// WHY THIS EXISTS NOW. The plan of record was to hang pre-UI antialiasing on the
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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>

#include "scene_target.h"
#include "../../core/highres.h"
#include "../../core/log.h"
#include "../../core/scene_pass.h"
#include "../../core/sharpen.h"
#include "../../core/smaa.h"
#include "../../core/supersample.h"
#include "../../core/d3d11_hooks.h"
#include "../../core/frame_map.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

constexpr UINT kColourBind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

// THE COLOUR TARGET IS TYPELESS, and this predicate is not optional -- the first
// run without it accepted `format=87 bind=0x28` at 380 ms, before any 3D scene
// exists. 87 is B8G8R8A8_UNORM, a typed presented surface; the scene colour is
// 90, B8G8R8A8_TYPELESS. Pre-UI SMAA duly engaged on the composite instead of
// the scene.
//
// Ayesha's test carries the same discriminator and its comment predicts this
// failure in almost these words. The reasoning transfers because it is about
// what a scene target IS rather than about which surfaces happen to differ: an
// engine allocates the surface it intends to both render into and sample back as
// typeless, so it can put a typed render-target view and a typed
// shader-resource view over one allocation. A presented surface is typed.
bool isTypeless(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return true;
    default:
      return false;
  }
}

bool ktglSceneTargets(const D3D11_TEXTURE2D_DESC& color,
                      const D3D11_TEXTURE2D_DESC& depth) {
  if (color.BindFlags != kColourBind)
    return false;
  if (!isTypeless(color.Format))
    return false;
  if (!(depth.BindFlags & D3D11_BIND_DEPTH_STENCIL))
    return false;
  if (color.SampleDesc.Count != 1 || depth.SampleDesc.Count != 1)
    return false;
  if (color.Width != depth.Width || color.Height != depth.Height)
    return false;

  unsigned int swapWidth = 0, swapHeight = 0;
  if (!highResSwapChainSize(&swapWidth, &swapHeight))
    return false;
  if (color.Width < swapWidth || color.Height < swapHeight)
    return false;

  // Said once, with both descriptors, because the record and the only census
  // disagree about the depth's bind flags and this is the line that settles it.
  static std::atomic<bool> announced{false};
  if (!announced.exchange(true, std::memory_order_relaxed))
    log("KTGL scene test: accepted colour ", std::dec, color.Width, "x",
        color.Height, " format=", unsigned(color.Format),
        " bind=0x", std::hex, color.BindFlags, std::dec,
        " with depth format=", unsigned(depth.Format),
        " bind=0x", std::hex, depth.BindFlags, std::dec,
        " (swap chain ", swapWidth, "x", swapHeight, ")");
  return true;
}

}  // namespace

// ---- the pre-UI anchor ----------------------------------------------------
//
// FOUND BY MAPPING A FRAME, after eight narrower probes each
// answered a slightly different question. One gameplay frame looks like this:
//
//   T0 T1 T2  59 draws          shadow pass
//   T1 T3 T1 184 draws          the 3D scene
//   T0 T1 T0 T4  15 draws       <-- draw 1 puts the finished scene in #4,
//                                   draws 2..15 are the INTERFACE
//   T0 T5 T6 ... 1 draw each    the post chain and blur pyramid
//   T5 1 draw, T0 1 draw        the final composite
//
// Surface #4 copied after its FIRST draw is the complete scene with no
// interface in it; the same surface at Present carries the date panel and the
// button prompts. So the pre-UI moment is exactly there, and it is the only
// place in the frame where a scene-only image exists after the scene is
// finished.
//
// THE RULE IS STRUCTURAL, not an address. The anchor is the first draw into a
// screen-sized typeless RENDER_TARGET|SHADER_RESOURCE colour surface that is
// bound AFTER the frame's main geometry run has finished. The geometry run is
// recognised by its size -- the scene is hundreds of draws and everything after
// it is ones and tens -- which is a property of how the frame is built rather
// than of this executable.
//
// Why not the Glow composite, which is easier to identify: it is post-UI. Its
// own render target holds the finished interface, so antialiasing there does
// what the Present-time pass already does and is why that one is opt-in.
namespace {

constexpr uint32_t kGeometryRunDraws = 100;

std::atomic<uint32_t> g_drawsThisBind{0};
std::atomic<uint32_t> g_largestRun{0};
std::atomic<bool> g_armed{false};
std::atomic<ID3D11Texture2D*> g_preUiSurface{nullptr};
std::atomic<bool> g_installed{false};

// RE-ENTRY GUARD, and it is not bookkeeping.
//
// The passes this anchor fires bind a render target and issue their own draws.
// Both go back through the hooks that feed the anchor: the bind re-arms it and
// the draw fires it, so the pass calls itself and blocks on the lock it is
// already holding. With edge smoothing off that hung Escha on the loading
// screen at 2.4 seconds -- the same failure smaa.cpp documents for its own
// path, and the reason its latch is claimed BEFORE it does any work rather
// than after.
//
// Thread-local because it answers "am I already inside the pass on THIS call
// stack", which is the question. A shared flag would also stand down a second
// thread that has every right to be here -- this engine records on several
// deferred contexts.
thread_local bool t_inPass = false;

}  // namespace

// Whether this engine's pre-UI anchor is in charge of the frame.
//
// The transition-based call in scene_pass.cpp has to stand down when it is.
// Both go through smaaApplySceneColor, which claims the frame with a
// once-per-frame latch, and the transition fires EARLIER -- so with both live
// the anchor was called on every frame and refused on every frame, and the only
// symptom was that nothing looked different.
bool ktglPreUiActive() {
  // Either pass keeps the anchor alive: sharpening does not need the smoothing
  // to have run, and turning edge smoothing off must not silently take the
  // sharpening with it.
  return g_installed.load(std::memory_order_relaxed) &&
         (smaaPreUiEnabled() || sharpenEnabled());
}

void ktglPreUiNoteTargets(unsigned int numViews,
                          ID3D11RenderTargetView* const* views) {
  // INERT UNLESS THIS ENGINE INSTALLED IT. These two are called from the shared
  // bind and draw detours, so without this check the anchor arms and fires on
  // Ayesha as well -- where the pre-UI pass is driven from scene_pass.cpp
  // instead. It did: Ayesha hung, because scene_pass's call was already inside
  // the sharpening pass when the anchor fired it a second time.
  if (!g_installed.load(std::memory_order_relaxed))
    return;
  if (t_inPass || !numViews || !views || !views[0])
    return;
  // Close the previous bind's run.
  const uint32_t run = g_drawsThisBind.exchange(0, std::memory_order_relaxed);
  uint32_t largest = g_largestRun.load(std::memory_order_relaxed);
  while (run > largest &&
         !g_largestRun.compare_exchange_weak(largest, run,
                                             std::memory_order_relaxed)) {}

  g_armed.store(false, std::memory_order_relaxed);
  g_preUiSurface.store(nullptr, std::memory_order_relaxed);
  if (g_largestRun.load(std::memory_order_relaxed) < kGeometryRunDraws)
    return;   // the scene has not been drawn yet this frame

  ID3D11Resource* resource = nullptr;
  views[0]->GetResource(&resource);
  if (!resource)
    return;
  ID3D11Texture2D* texture = nullptr;
  resource->QueryInterface(IID_ID3D11Texture2D,
                           reinterpret_cast<void**>(&texture));
  resource->Release();
  if (!texture)
    return;
  D3D11_TEXTURE2D_DESC d = {};
  texture->GetDesc(&d);
  unsigned int swapWidth = 0, swapHeight = 0;
  if (d.BindFlags == kColourBind && isTypeless(d.Format) &&
      highResSwapChainSize(&swapWidth, &swapHeight) &&
      d.Width >= swapWidth && d.Height >= swapHeight) {
    g_armed.store(true, std::memory_order_relaxed);
    g_preUiSurface.store(texture, std::memory_order_relaxed);

    // WHICH SURFACE THIS ACTUALLY IS, reported once per distinct size.
    //
    // The rule above is a size-and-format rule, and with supersampling on it
    // matches more than one surface: an Escha run alternated
    // between 3840x2160 and 2560x1440 several times a minute, reinitialising
    // the SMAA targets at every switch. A size rule cannot say which of the two
    // is the scene. The tags can -- one is set by the engine's own scene test,
    // the other by the swap chain -- so they are printed here rather than
    // guessed at, and whichever they name is what the rule should be keyed on.
    static std::atomic<uint32_t> lastWidth{0};
    static std::atomic<uint32_t> lastHeight{0};
    const uint32_t previousWidth =
      lastWidth.exchange(d.Width, std::memory_order_relaxed);
    const uint32_t previousHeight =
      lastHeight.exchange(d.Height, std::memory_order_relaxed);
    if (previousWidth != d.Width || previousHeight != d.Height)
      log("KTGL pre-UI: armed on ", std::dec, d.Width, "x", d.Height,
          " format=", uint32_t(d.Format), " bind=0x", std::hex, d.BindFlags,
          std::dec, " after a run of ",
          g_largestRun.load(std::memory_order_relaxed), " draws",
          " -- sceneHostTag=", ssaaIsSceneHost(texture) ? "yes" : "no",
          " backBufferTag=", ssaaIsBackBuffer(texture) ? "yes" : "no");
  }
  texture->Release();
}

// Returns the surface to antialias once, on the first draw after arming.
ID3D11Texture2D* ktglPreUiNoteDraw() {
  if (!g_installed.load(std::memory_order_relaxed) || t_inPass)
    return nullptr;
  const uint32_t n = g_drawsThisBind.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n != 1 || !g_armed.exchange(false, std::memory_order_relaxed))
    return nullptr;
  return g_preUiSurface.load(std::memory_order_relaxed);
}

// ---- the draw detours this feature owns ------------------------------------
//
// The pre-UI pass needs to fire immediately AFTER a draw, so it owns the draw
// slots rather than borrowing them from a diagnostic. It briefly did borrow the
// Glow trace's, which made a shipped feature depend on an environment switch.
//
// The trace and the frame map are notified from here so there is still exactly
// one set of draw detours on the vtable.

// REACHABLE FROM TWO PLACES, and it has to be.
//
// This feature owns the four draw slots when nothing else wants them. When
// supersampling is on, the raster correction owns them instead -- one vtable
// slot cannot hold two detours -- and d3d11_hooks.cpp declines this set. That
// used to mean the pre-UI pass silently did nothing in every supersampled
// session, so a Shallie shipping default of `Supersampling=150, Sharpen=75`
// applied no sharpening at all and said so nowhere.
//
// So the anchor is a function rather than something buried in the detours, and
// ScenePolicy::afterDraw points at it. Whichever family holds the slots calls
// it; only one family is ever installed, so it cannot fire twice.
void ktglPreUiAfterDraw(ID3D11DeviceContext* self) {
  // NOT WHEN SUPERSAMPLING HAS ENGAGED. The downscale runs both passes itself,
  // on the display-sized result and still before the interface -- see the
  // comment at the smaaApplySceneColor call in supersample.cpp, which gives the
  // three reasons that placement is better. Running here as well does not
  // double the antialiasing, it ALTERNATES with it: both claim the same
  // once-per-frame latch, so whichever reaches it first wins that frame. An
  // Escha run flipped between 3840x2160 and 2560x1440 several
  // times a minute, reinitialising SMAA's targets at every switch.
  //
  // Engaged, not configured: supersampling that is switched on but never
  // attaches must not take this pass down with it.
  if (ssaaEngaged())
    return;
  if (ID3D11Texture2D* preUi = ktglPreUiNoteDraw()) {
    // Claimed BEFORE either pass runs, so their own binds and draws cannot come
    // back round and re-enter this.
    t_inPass = true;
    const bool ran = smaaApplySceneColor(self, preUi);
    // AFTER the antialiasing when there is any, and REGARDLESS of whether there
    // was. Sharpening before would only give SMAA harder edges to blend away
    // again, but it does not depend on that pass having run -- with edge
    // smoothing off this is simply a sharpening filter on the finished scene,
    // still applied before the interface so menu text is never touched.
    const bool sharpened = sharpenApply(self, preUi);

    // ANNOUNCED ON THE FIRST SUCCESS, not on the first call. The first calls of
    // a session happen while a save is loading, where the pass legitimately
    // declines -- latching on that reported "REFUSED" for a feature that then
    // worked for the rest of the run.
    static std::atomic<bool> said{false};
    static std::atomic<uint64_t> refusals{0};
    if (ran || sharpened) {
      if (!said.exchange(true, std::memory_order_relaxed))
        log("KTGL pre-UI: active -- smoothing=", ran ? "on" : "off",
            " sharpening=", sharpened ? "on" : "off",
            ", both applied to the interface target before the interface is"
            " drawn into it (", std::dec,
            refusals.load(std::memory_order_relaxed),
            " earlier frames declined, which is normal during a load)");
    } else {
      refusals.fetch_add(1, std::memory_order_relaxed);
    }
    t_inPass = false;
  }
}

void STDMETHODCALLTYPE hookedPreUiDraw(ID3D11DeviceContext* self,
                                       UINT vertexCount, UINT startVertex) {
  frameMapNoteDraw(self);
  d3d11OriginalsFor(self).draw(self, vertexCount, startVertex);
  ktglPreUiAfterDraw(self);
}

void STDMETHODCALLTYPE hookedPreUiDrawIndexed(ID3D11DeviceContext* self,
                                              UINT indexCount, UINT startIndex,
                                              INT baseVertex) {
  frameMapNoteDraw(self);
  d3d11OriginalsFor(self).drawIndexed(self, indexCount, startIndex, baseVertex);
  ktglPreUiAfterDraw(self);
}

void STDMETHODCALLTYPE hookedPreUiDrawIndexedInstanced(
    ID3D11DeviceContext* self, UINT indexCountPerInstance, UINT instanceCount,
    UINT startIndex, INT baseVertex, UINT startInstance) {
  frameMapNoteDraw(self);
  d3d11OriginalsFor(self).drawIndexedInstanced(self, indexCountPerInstance,
    instanceCount, startIndex, baseVertex, startInstance);
  ktglPreUiAfterDraw(self);
}

void STDMETHODCALLTYPE hookedPreUiDrawInstanced(
    ID3D11DeviceContext* self, UINT vertexCountPerInstance, UINT instanceCount,
    UINT startVertex, UINT startInstance) {
  frameMapNoteDraw(self);
  d3d11OriginalsFor(self).drawInstanced(self, vertexCountPerInstance,
    instanceCount, startVertex, startInstance);
  ktglPreUiAfterDraw(self);
}

void ktglPreUiFrameTick() {
  g_drawsThisBind.store(0, std::memory_order_relaxed);
  g_largestRun.store(0, std::memory_order_relaxed);
  g_armed.store(false, std::memory_order_relaxed);
  g_preUiSurface.store(nullptr, std::memory_order_relaxed);
}

void installKtglSceneTarget() {
  scenePassSetTest(&ktglSceneTargets);
  g_installed.store(true, std::memory_order_relaxed);
  log("KTGL scene test: registered -- the pre-UI boundary is now identifiable"
      " on this engine");
}

}  // namespace atfix
