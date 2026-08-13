// SPDX-License-Identifier: MIT
//
// See highres.h for what this fixes and why the fix and its census share a file.
//
// PROVENANCE. The mechanism is TellowKrinkle's, from the rendering branch of
// his atelier-sync-fix fork, whose CreateTexture2D comment names Ayesha
// explicitly among the games that "always render at 1080p no matter the
// requested resolution". The refinements are the Arland project's: the shape
// validation on the main-target guess, the half-size blur-target rule, and
// keeping raster state per context. Both are prior art this project is meant to
// lean on rather than re-derive. Yuri Hime's Atelier Graphics Tweak also ships
// a resolution hack for these games; none of its code is used here, and it was
// not consulted for this.
//
// This file owns the resolution fix, the census that measured the need for it,
// and the detours both work through -- but NOT the vtables those detours are
// installed into. d3d11_hooks.cpp owns those, and owns which of them go in.
// See that header for why one module holds them: this file used to, and ended
// up hosting detours belonging to features it has nothing to do with.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "config.h"
#include "game.h"
#include "highres.h"
#include "scene_policy.h"
#include "log.h"
#include "supersample.h"
#include "util.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// Matches the cadence of the other periodic diagnostics, so two of them
// running together interleave predictably.
constexpr uint64_t kReportInterval = 300;

// The size the engine hard-codes, and the blur target derived from it. Both are
// literals in the game rather than anything it computes, which is why matching
// them literally is the right rule and not a heuristic.
constexpr UINT kPinnedWidth  = 1920;
constexpr UINT kPinnedHeight = 1080;
constexpr UINT kBlurWidth    = 960;
constexpr UINT kBlurHeight   = 540;

bool g_fixEnabled = false;
bool g_censusEnabled = false;
bool g_installed = false;

// Width and height are one fact, so publish them in one atomic value. Separate
// relaxed atomics allowed a reader to observe the width from one publication
// and the height from another; that is not a C++ data race, but it is not a
// coherent resolution either.
uint64_t packSize(UINT width, UINT height) {
  return (uint64_t(width) << 32) | uint64_t(height);
}

bool unpackSize(uint64_t packed, UINT* width, UINT* height) {
  if (!packed)
    return false;
  *width = UINT(packed >> 32);
  *height = UINT(packed);
  return *width && *height;
}

std::atomic<uint64_t> g_mainSize{0};
std::atomic<uint64_t> g_swapSize{0};

// ---- the census ------------------------------------------------------------

uintptr_t censusModuleBase() {
  static const uintptr_t base =
    reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  return base;
}

// Must be evaluated in the hook body itself, never one call deeper:
// duskReturnAddress() is frame-based, so only the function MinHook redirects
// sees the game's own return address. A macro is what guarantees that.
//
// Worth knowing what this actually names here: every one of these creations
// arrives through the engine's generic texture-creation wrapper (0x559600 in
// the English build), which builds the descriptor from its arguments and calls
// the device through a vtable slot. So the caller RVA identifies the wrapper,
// not whoever chose the size. It is still worth logging -- a second distinct
// caller appearing would mean the engine has a creation path this was never
// measured against -- but it is not an attribution of the hard-coded 1080p.
#define censusCallerRva() \
  uintptr_t(reinterpret_cast<uintptr_t>(duskReturnAddress()) - \
            censusModuleBase())

struct ShapeKey {
  uint32_t  width;
  uint32_t  height;
  uint32_t  format;
  uint32_t  bindFlags;
  uint32_t  sampleCount;
  uintptr_t callerRva;

  bool operator == (const ShapeKey& o) const {
    return width == o.width && height == o.height && format == o.format &&
           bindFlags == o.bindFlags && sampleCount == o.sampleCount &&
           callerRva == o.callerRva;
  }
};

struct ShapeKeyHash {
  size_t operator () (const ShapeKey& k) const {
    size_t h = std::hash<uint64_t>()(
      (uint64_t(k.width) << 32) ^ uint64_t(k.height));
    h ^= std::hash<uint32_t>()(k.format) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>()(k.bindFlags) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>()(k.sampleCount) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
    h ^= std::hash<uintptr_t>()(k.callerRva) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
    return h;
  }
};

// Generous, because the whole point is to enumerate: Ayesha settles at 15
// distinct shapes, so hitting this cap means something is wrong with the key
// rather than with the game.
constexpr size_t kMaxShapes = 4096;
bool g_shapesOverflowed = false;

atfix::mutex g_shapesMutex;
std::unordered_map<ShapeKey, uint64_t, ShapeKeyHash> g_shapes;  // guarded
std::atomic<uint64_t> g_totalTargets{0};

bool isTarget(const D3D11_TEXTURE2D_DESC& desc) {
  return (desc.BindFlags &
          (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0;
}

// `action` names what the fix did with this creation, so one log line carries
// both the shape and its disposition and the two never have to be correlated
// by hand.
void censusReport(const D3D11_TEXTURE2D_DESC& desc, uintptr_t callerRva,
                  const char* action) {
  const ShapeKey key{ desc.Width, desc.Height, uint32_t(desc.Format),
                      desc.BindFlags, desc.SampleDesc.Count, callerRva };
  uint64_t count = 0;
  bool firstSeen = false;
  {
    std::lock_guard lock(g_shapesMutex);
    if (g_shapes.size() >= kMaxShapes && !g_shapes.count(key)) {
      // Stop growing rather than stop counting; the total stays exact.
      g_shapesOverflowed = true;
      g_totalTargets.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    uint64_t& c = g_shapes[key];
    firstSeen = c == 0;
    count = ++c;
  }
  g_totalTargets.fetch_add(1, std::memory_order_relaxed);
  if (!firstSeen)
    return;

  UINT swapWidth = 0;
  UINT swapHeight = 0;
  unpackSize(g_swapSize.load(std::memory_order_acquire),
             &swapWidth, &swapHeight);
  const char* relation =
    (swapWidth && desc.Width == swapWidth && desc.Height == swapHeight)
      ? " rel=matchesSwapChain"
      : (desc.Width == kPinnedWidth && desc.Height == kPinnedHeight)
          ? " rel=1920x1080" : " rel=other";

  log("TARGETCENSUS ", std::dec, desc.Width, "x", desc.Height,
      relation,
      " format=", uint32_t(desc.Format),
      " samples=", desc.SampleDesc.Count,
      " mips=", desc.MipLevels,
      " arraySize=", desc.ArraySize,
      " usage=", uint32_t(desc.Usage),
      " bindFlags=0x", std::hex, desc.BindFlags, std::dec,
      " callerRva=0x", std::hex, callerRva, std::dec,
      " action=", action,
      " count=", count);
}

// ---- the fix ---------------------------------------------------------------

// The first depth-stencil target the game creates is its main one, and it is
// created at the requested resolution. That ordering is what the whole fix
// rests on, so it is checked rather than trusted: the candidate must be 16:9
// and at least 1920x1080, or match the size the swap chain was created at.
//
// Ayesha's observed order is exactly this -- a 2560x1440 DEPTH_STENCIL at
// 125 ms, the first hard-coded 1920x1080 target at 2241 ms -- and the shape
// check is what stops a game that reordered its creations from having some
// unrelated small depth buffer adopted as the main size and every scene target
// resized to it.
bool looksLikeMainTarget(const D3D11_TEXTURE2D_DESC& desc) {
  if (!(desc.BindFlags & D3D11_BIND_DEPTH_STENCIL))
    return false;
  UINT swapWidth = 0;
  UINT swapHeight = 0;
  unpackSize(g_swapSize.load(std::memory_order_acquire),
             &swapWidth, &swapHeight);
  if (swapWidth && swapHeight &&
      desc.Width == swapWidth && desc.Height == swapHeight)
    return true;
  return desc.Width >= kPinnedWidth && desc.Height >= kPinnedHeight &&
         uint64_t(desc.Width) * 9 == uint64_t(desc.Height) * 16;
}

// The engine's hard-coded full-size target: exactly 1920x1080, bound as a
// render target or depth-stencil, and created empty. The initial-data check is
// what keeps a texture that merely happens to be that size -- a loaded image,
// say -- from being caught by accident.
bool isPinnedFullTarget(const D3D11_TEXTURE2D_DESC& desc,
                        const D3D11_SUBRESOURCE_DATA* data) {
  return !data && isTarget(desc) &&
         desc.Width == kPinnedWidth && desc.Height == kPinnedHeight;
}

// The half-size blur target, carried over from the Arland implementation where
// it was needed for the same reason: the engine hard-codes 960x540 for its blur
// chain, so leaving it behind gives a blur pass sampled at 1080p proportions
// over a 1440p scene. Narrowly specified on purpose -- colour only, typeless
// BGRA, single-sample, no mips, no array -- because "half of the pinned size" is a
// shape plenty of unrelated textures could share.
bool isPinnedBlurTarget(const D3D11_TEXTURE2D_DESC& desc,
                        const D3D11_SUBRESOURCE_DATA* data) {
  return !data &&
         (desc.BindFlags & D3D11_BIND_RENDER_TARGET) &&
         (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
         desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
         desc.Width == kBlurWidth && desc.Height == kBlurHeight &&
         desc.MipLevels == 1 && desc.ArraySize == 1 &&
         desc.SampleDesc.Count == 1;
}

}  // namespace

HRESULT STDMETHODCALLTYPE hookedCreateTexture2D(
    ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Texture2D** texture) {
  if (!desc)
    return d3d11DeviceOriginals().createTexture2D(self, desc, initialData, texture);

  D3D11_TEXTURE2D_DESC local = *desc;
  const char* action = "passthrough";

  if (g_fixEnabled) {
    uint64_t noMainSize = 0;
    if (looksLikeMainTarget(local) &&
        g_mainSize.compare_exchange_strong(
          noMainSize, packSize(local.Width, local.Height),
          std::memory_order_release, std::memory_order_relaxed)) {
      log("HIGHRES: main render size ", std::dec, local.Width, "x",
          local.Height);
      action = "adoptedAsMain";
    } else {
      // The ONE definition of how big the scene targets are. Not recomputed
      // here from the main size and a factor: the last time this arithmetic
      // existed in two places -- once here and once in the scene test --
      // enabling supersampling made them disagree, the test stopped matching,
      // and the features that depend on it silently did nothing for a whole
      // session while every log line looked healthy. See highResSceneSize.
      unsigned int sceneWidth = 0;
      unsigned int sceneHeight = 0;
      const bool haveScene = highResSceneSize(&sceneWidth, &sceneHeight);
      const bool supersampled = haveScene && ssaaConfigured();

      // Only ever scales up. At or below the pinned size there is nothing to
      // correct, and rewriting anyway would mean an ordinary 1080p session
      // taking a different path from the one the game shipped with. With
      // supersampling on, the SCENE size is what has to clear that bar -- a
      // 1080p display at 200% wants the enlargement even though its main size
      // does not exceed the pinned one.
      if (haveScene && sceneWidth > kPinnedWidth && sceneHeight > kPinnedHeight) {
        if (isPinnedFullTarget(local, initialData)) {
          local.Width = sceneWidth;
          local.Height = sceneHeight;
          action = supersampled ? "resizedFull+ssaa" : "resizedFull";
        } else if (isPinnedBlurTarget(local, initialData)) {
          // The blur ladder is defined relative to the scene it blurs, not to
          // the display, so it scales with the scene target. The engine's own
          // shaders carry no screen-size constant and no hard-coded resolution
          // literal -- all 139 of its DXBC containers were scanned for both --
          // so a larger ladder is expected to be transparent to them. Expected,
          // not proven: watch bloom placement on a fractional factor.
          local.Width = sceneWidth / 2;
          local.Height = sceneHeight / 2;
          action = supersampled ? "resizedBlur+ssaa" : "resizedBlur";
        }
      }
    }
  }

  // DO NOT ADD MULTISAMPLING BACK HERE. An early version rode this hook and
  // raised the sample count on targets the engine had already created
  // multisampled, on the reading that the game "already renders 4x MSAA".
  // Those targets are never rendered into. The census reports what the game
  // CREATES, and the six 4-sample targets it allocates at startup and abandons
  // are indistinguishable, at creation time, from six it uses. Draw
  // instrumentation settled it: `drawsToMsaa=0` and `maxBoundSamples=0` over
  // 7200 frames and 117245 sampled draws. The twin implementation that replaced
  // it has since been removed too -- multisampling cannot reach what actually
  // aliases in these games, which is detail inside textures and alpha-tested
  // trim, and only supersampling resolves that.

  // Reported before forwarding and from the descriptor the game asked for, so
  // the census keeps saying what the game wanted; `action` says what it got.
  if (g_censusEnabled && isTarget(*desc))
    censusReport(*desc, censusCallerRva(), action);

  const HRESULT hr =
    d3d11DeviceOriginals().createTexture2D(self, &local, initialData, texture);
  // Never hide a refused resize by substituting the game's original-size
  // descriptor. Related targets must agree on their dimensions, and by this
  // point earlier members may already have been created at the enlarged size;
  // the hook cannot roll those allocations back. Returning the driver's
  // failure leaves recovery with the engine, which can abandon the operation,
  // instead of manufacturing a mixed-size family behind its back. This is the
  // same fail-closed policy used by the Arland high-resolution path.
  if (FAILED(hr) &&
      (local.Width != desc->Width || local.Height != desc->Height ||
       local.SampleDesc.Count != desc->SampleDesc.Count))
    log("HIGHRES: resized allocation ", std::dec, local.Width, "x",
        local.Height, " was refused (hr=0x", std::hex, uint32_t(hr),
        std::dec, "); returning the failure without an incompatible ",
        desc->Width, "x", desc->Height, " fallback");
  return hr;
}

namespace {

// ---- raster state ---------------------------------------------------------
//
// The engine submits a hard-coded full-screen 1920x1080 viewport and scissor to
// go with its hard-coded targets, so both have to move with them.
//
// This was first written to rewrite them eagerly, in RSSetViewports and
// RSSetScissorRects, on the reasoning that the resize had already moved every
// exact-1920x1080 target so nothing legitimate could still want a 1080p
// full-screen viewport. That was wrong, and a run said so: the scene came out
// complete and correctly proportioned but drawn into roughly nine-sixteenths of
// the window, which is the signature of a scale applied where it did not
// belong. Some pass submits that viewport while bound to something the resize
// did not touch.
//
// So this now does what TellowKrinkle's fork and the Arland implementation both
// do, and for the reason they do it: record the submission, then fix it up at
// the next draw, when the render target is actually bound and can be asked how
// big it is. The four draw hooks that costs are the price of the correction
// being conditional on the thing it depends on.
// Raster dirtiness belongs to the context whose command stream submitted it.
// A single "deferred" bucket was safe only if all deferred contexts serialized
// their RS and draw calls. The games create replacement deferred contexts, and
// the hook surface explicitly accepts more than one, so hang the bit on the
// D3D object itself. Its lifetime then follows context destruction and pointer
// reuse cannot inherit an earlier context's pending update.
const GUID IID_DuskHighResRasterDirty =
  { 0x52e91c75, 0x620a, 0x45c7,
    { 0x97, 0x71, 0xe4, 0xf6, 0xe1, 0x7d, 0xb8, 0x0a } };

void markRasterDirty(ID3D11DeviceContext* context) {
  const UINT dirty = 1;
  context->SetPrivateData(IID_DuskHighResRasterDirty, sizeof(dirty), &dirty);
}

bool takeRasterDirty(ID3D11DeviceContext* context) {
  UINT dirty = 0;
  UINT size = sizeof(dirty);
  if (FAILED(context->GetPrivateData(IID_DuskHighResRasterDirty,
                                     &size, &dirty)) || !dirty)
    return false;
  context->SetPrivateData(IID_DuskHighResRasterDirty, 0, nullptr);
  return true;
}


// The size of whatever is bound as render target 0, or depth-stencil if there
// is no colour target. Nothing is assumed about which of the two is present.
// Draws counted by the sample count of the target they landed on. This is the
// only thing that answers "is the scene actually multisampled" -- the census
// reports targets the game CREATES, and a 4-sample texture that nothing renders
// into proves nothing about the picture on screen.
std::atomic<uint64_t> g_drawsSingleSample{0};
std::atomic<uint64_t> g_drawsMultiSample{0};
std::atomic<uint64_t> g_maxBoundSamples{0};

bool boundTargetSize(ID3D11DeviceContext* context, UINT* width, UINT* height) {
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  context->OMGetRenderTargets(1, &rtv, &dsv);
  ID3D11Resource* resource = nullptr;
  if (rtv)
    rtv->GetResource(&resource);
  if (!resource && dsv)
    dsv->GetResource(&resource);
  bool found = false;
  if (resource) {
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_ID3D11Texture2D,
                                           reinterpret_cast<void**>(&texture))) &&
        texture) {
      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      *width = desc.Width;
      *height = desc.Height;
      found = true;
      if (desc.SampleDesc.Count > 1) {
        g_drawsMultiSample.fetch_add(1, std::memory_order_relaxed);
        uint64_t previous = g_maxBoundSamples.load(std::memory_order_relaxed);
        while (desc.SampleDesc.Count > previous &&
               !g_maxBoundSamples.compare_exchange_weak(previous,
                 desc.SampleDesc.Count, std::memory_order_relaxed)) {
        }
      } else {
        g_drawsSingleSample.fetch_add(1, std::memory_order_relaxed);
      }
      texture->Release();
    }
    resource->Release();
  }
  if (rtv)
    rtv->Release();
  if (dsv)
    dsv->Release();
  return found;
}

std::atomic<uint64_t> g_viewportRewrites{0};
std::atomic<uint64_t> g_scissorRewrites{0};

// Counters that exist to localize a correction that does not fire.
//
// Two attempts at the raster correction produced zero rewrites, and the reason
// could not be told apart from the outside: "the hook never runs", "the hook
// runs but the state is never dirty", "the state is dirty but the bound target
// cannot be read" and "everything works but the sizes do not match" all look
// identical in a log that only counts successes. Each step now has its own
// number, so one run says which one it is.
std::atomic<uint64_t> g_rsViewportCalls{0};
std::atomic<uint64_t> g_rsScissorCalls{0};
std::atomic<uint64_t> g_drawCalls{0};
std::atomic<uint64_t> g_updatesEntered{0};   // draws that found dirty state
std::atomic<uint64_t> g_targetLookupFails{0};

// The first few distinct (viewport, scissor, bound target) combinations, logged
// once each. This is the line that says what the engine actually submits and
// what it is bound to when it does, which is the fact every version of this fix
// has been assuming rather than measuring.
constexpr int kMaxRasterSamples = 12;
atfix::mutex g_rasterSampleMutex;
int g_rasterSampleCount = 0;
struct RasterSample {
  float viewportWidth, viewportHeight, viewportX, viewportY;
  LONG  scissorRight, scissorBottom;
  UINT  targetWidth, targetHeight;
  bool operator == (const RasterSample& o) const {
    return viewportWidth == o.viewportWidth && viewportHeight == o.viewportHeight &&
           viewportX == o.viewportX && viewportY == o.viewportY &&
           scissorRight == o.scissorRight && scissorBottom == o.scissorBottom &&
           targetWidth == o.targetWidth && targetHeight == o.targetHeight;
  }
};
RasterSample g_rasterSamples[kMaxRasterSamples] = {};

void sampleRaster(const D3D11_VIEWPORT& viewport, const D3D11_RECT& scissor,
                  UINT targetWidth, UINT targetHeight, bool haveTarget) {
  if (!g_censusEnabled)
    return;
  const RasterSample sample{ viewport.Width, viewport.Height,
    viewport.TopLeftX, viewport.TopLeftY, scissor.right, scissor.bottom,
    haveTarget ? targetWidth : 0, haveTarget ? targetHeight : 0 };
  {
    std::lock_guard lock(g_rasterSampleMutex);
    if (g_rasterSampleCount >= kMaxRasterSamples)
      return;
    for (int i = 0; i < g_rasterSampleCount; ++i) {
      if (g_rasterSamples[i] == sample)
        return;
    }
    g_rasterSamples[g_rasterSampleCount++] = sample;
  }
  log("HIGHRES RASTER viewport=", std::dec, UINT(viewport.Width), "x",
      UINT(viewport.Height), "@", UINT(viewport.TopLeftX), ",",
      UINT(viewport.TopLeftY),
      " scissor=", LONG(scissor.right), "x", LONG(scissor.bottom),
      haveTarget ? " boundTarget=" : " boundTarget=none",
      haveTarget ? std::to_string(targetWidth) + "x" +
                   std::to_string(targetHeight) : std::string());
}

// Called from every draw whose context has raster state pending. Rewrites a
// full-screen viewport and scissor to the bound target's real size, and only
// when that target is genuinely larger than what was submitted -- which is what
// makes this conditional on the thing the eager version assumed.
void updateViewportScissor(ID3D11DeviceContext* context) {
  if (!takeRasterDirty(context))
    return;
  g_updatesEntered.fetch_add(1, std::memory_order_relaxed);

  UINT viewportCount = 1;
  UINT scissorCount = 1;
  D3D11_VIEWPORT viewport = {};
  D3D11_RECT scissor = {};
  context->RSGetViewports(&viewportCount, &viewport);
  context->RSGetScissorRects(&scissorCount, &scissor);

  // Only a single full-screen-from-the-origin viewport or scissor is a
  // candidate. A partial one belongs to a pass that meant it.
  const bool viewportCandidate = viewportCount == 1 &&
    viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f &&
    viewport.Width > 0.0f && viewport.Height > 0.0f;
  const bool scissorCandidate = scissorCount == 1 &&
    scissor.left == 0 && scissor.top == 0 &&
    scissor.right > 0 && scissor.bottom > 0;
  if (!viewportCandidate && !scissorCandidate)
    return;

  UINT targetWidth = 0;
  UINT targetHeight = 0;
  const bool haveTarget = boundTargetSize(context, &targetWidth, &targetHeight);
  sampleRaster(viewport, scissor, targetWidth, targetHeight, haveTarget);
  if (!haveTarget) {
    g_targetLookupFails.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  bool rewroteViewport = false;
  if (viewportCandidate &&
      UINT(viewport.Width) < targetWidth && UINT(viewport.Height) < targetHeight) {
    // The submitted viewport is smaller than the surface it will draw into, on
    // a pass that asked for the whole surface. That is precisely the engine's
    // hard-coded size meeting a target the resize enlarged, and nothing else
    // this renderer does. MinDepth and MaxDepth are never touched: a pass that
    // remaps depth is not one whose size we are correcting.
    viewport.Width = float(targetWidth);
    viewport.Height = float(targetHeight);
    rewroteViewport = true;
  }
  bool rewroteScissor = false;
  if (scissorCandidate &&
      UINT(scissor.right) < targetWidth && UINT(scissor.bottom) < targetHeight) {
    scissor.right = LONG(targetWidth);
    scissor.bottom = LONG(targetHeight);
    rewroteScissor = true;
  }

  const ContextOriginals& originals = d3d11OriginalsFor(context);
  if (rewroteViewport && originals.rsSetViewports) {
    originals.rsSetViewports(context, 1, &viewport);
    g_viewportRewrites.fetch_add(1, std::memory_order_relaxed);
  }
  if (rewroteScissor && originals.rsSetScissorRects) {
    originals.rsSetScissorRects(context, 1, &scissor);
    g_scissorRewrites.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

void STDMETHODCALLTYPE hookedRSSetViewports(
    ID3D11DeviceContext* self, UINT count, const D3D11_VIEWPORT* viewports) {
  d3d11OriginalsFor(self).rsSetViewports(self, count, viewports);
  g_rsViewportCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    markRasterDirty(self);
}

void STDMETHODCALLTYPE hookedRSSetScissorRects(
    ID3D11DeviceContext* self, UINT count, const D3D11_RECT* rects) {
  d3d11OriginalsFor(self).rsSetScissorRects(self, count, rects);
  g_rsScissorCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    markRasterDirty(self);
}

// The four draw entry points. Each does the same thing: settle any pending
// raster state, then forward. The dirty flag means the work above runs at most
// once per raster change rather than once per draw.
void STDMETHODCALLTYPE hookedDraw(
    ID3D11DeviceContext* self, UINT vertexCount, UINT startVertex) {
  g_drawCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    updateViewportScissor(self);
  // The other engine's correction. Ayesha's is above and enlarges the viewport
  // to match targets this module grew; KTGL's shrinks it to match a back buffer
  // supersample.cpp clamped. Exactly one of the two ever does anything.
  ssaaCorrectCompositeViewport(self);
  d3d11OriginalsFor(self).draw(self, vertexCount, startVertex);
  // The pre-UI anchor, when this engine has one that fires from draws.
  // Empty otherwise. See core/scene_policy.h for why it is reached from
  // here rather than from detours of its own.
  scenePolicy().afterDraw(self);
}

void STDMETHODCALLTYPE hookedDrawIndexed(
    ID3D11DeviceContext* self, UINT indexCount, UINT startIndex,
    INT baseVertex) {
  g_drawCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    updateViewportScissor(self);
  // The other engine's correction. Ayesha's is above and enlarges the viewport
  // to match targets this module grew; KTGL's shrinks it to match a back buffer
  // supersample.cpp clamped. Exactly one of the two ever does anything.
  ssaaCorrectCompositeViewport(self);
  d3d11OriginalsFor(self).drawIndexed(self, indexCount, startIndex, baseVertex);
  // The pre-UI anchor, when this engine has one that fires from draws.
  // Empty otherwise. See core/scene_policy.h for why it is reached from
  // here rather than from detours of its own.
  scenePolicy().afterDraw(self);
}

void STDMETHODCALLTYPE hookedDrawInstanced(
    ID3D11DeviceContext* self, UINT vertexCountPerInstance, UINT instanceCount,
    UINT startVertex, UINT startInstance) {
  g_drawCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    updateViewportScissor(self);
  // The other engine's correction. Ayesha's is above and enlarges the viewport
  // to match targets this module grew; KTGL's shrinks it to match a back buffer
  // supersample.cpp clamped. Exactly one of the two ever does anything.
  ssaaCorrectCompositeViewport(self);
  d3d11OriginalsFor(self).drawInstanced(self, vertexCountPerInstance, instanceCount,
                                   startVertex, startInstance);
  // The pre-UI anchor, when this engine has one that fires from draws.
  // Empty otherwise. See core/scene_policy.h for why it is reached from
  // here rather than from detours of its own.
  scenePolicy().afterDraw(self);
}

void STDMETHODCALLTYPE hookedDrawIndexedInstanced(
    ID3D11DeviceContext* self, UINT indexCountPerInstance, UINT instanceCount,
    UINT startIndex, INT baseVertex, UINT startInstance) {
  g_drawCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    updateViewportScissor(self);
  // The other engine's correction. Ayesha's is above and enlarges the viewport
  // to match targets this module grew; KTGL's shrinks it to match a back buffer
  // supersample.cpp clamped. Exactly one of the two ever does anything.
  ssaaCorrectCompositeViewport(self);
  d3d11OriginalsFor(self).drawIndexedInstanced(self, indexCountPerInstance,
                                          instanceCount, startIndex,
                                          baseVertex, startInstance);
  // The pre-UI anchor, when this engine has one that fires from draws.
  // Empty otherwise. See core/scene_policy.h for why it is reached from
  // here rather than from detours of its own.
  scenePolicy().afterDraw(self);
}

HighResWants highResResolveWants() {
  g_fixEnabled = featureEnabled(Feature::HighResRendering);
  g_censusEnabled = featureEnabled(Feature::TargetCensus);
  return HighResWants{ g_fixEnabled || g_censusEnabled, g_fixEnabled };
}

void highResNoteImmediateContext(ID3D11DeviceContext* context) {
  (void)context;
  // Called only once the install has fully succeeded, which is what makes it
  // the right place to latch this: the census summary is gated on it, and a
  // summary reporting "nothing found" after a failed install would be
  // indistinguishable from one reporting a genuinely quiet frame.
  g_installed = true;
}

bool highResSceneSize(unsigned int* width, unsigned int* height) {
  // THE SOLE OWNER of "how big are the scene targets". Every consumer goes
  // through here: the resize in hookedCreateTexture2D, the gate that decides
  // whether to resize at all, the half-size blur target, and Ayesha's scene
  // test in src/engines/phyre/scene_target.cpp. supersample.cpp owns the
  // factor and the clamp; it does not own this answer.
  unsigned int mainWidth = 0;
  unsigned int mainHeight = 0;
  if (!highResMainSize(&mainWidth, &mainHeight))
    return false;

  unsigned int sceneWidth = mainWidth;
  unsigned int sceneHeight = mainHeight;
  ssaaSceneSize(mainWidth, mainHeight, &sceneWidth, &sceneHeight);

  // One line, once, naming both halves of the fact and the factor between them.
  // Logged from here rather than from either consumer, so what the log says is
  // by construction what the consumers were given.
  static std::atomic<bool> announced{false};
  if (!announced.exchange(true, std::memory_order_relaxed))
    log("HIGHRES: scene size ", std::dec, sceneWidth, "x", sceneHeight,
        " = main ", mainWidth, "x", mainHeight, " x ", ssaaPercent(), "%");

  *width = sceneWidth;
  *height = sceneHeight;
  return true;
}

bool highResMainSize(unsigned int* width, unsigned int* height) {
  return unpackSize(g_mainSize.load(std::memory_order_acquire), width, height);
}

bool highResSwapChainSize(unsigned int* width, unsigned int* height) {
  return unpackSize(g_swapSize.load(std::memory_order_acquire), width, height);
}

void noteSwapChainSize(unsigned int width, unsigned int height,
                       unsigned int format, unsigned int refreshNumerator,
                       unsigned int refreshDenominator, bool windowed) {
  g_swapSize.store(packSize(width, height), std::memory_order_release);
  static bool logged = false;
  if (logged)
    return;
  logged = true;
  log("Swap chain: ", std::dec, width, "x", height,
      " format=", format,
      " refresh=", refreshNumerator, "/", refreshDenominator,
      windowed ? " windowed" : " fullscreen");
}

void highResFrameTick() {
  if (!g_installed || !g_censusEnabled)
    return;
  static uint64_t frame = 0;
  ++frame;
  // The early report exists so a run confirms within seconds that the census is
  // live, instead of looking silent until the first interval elapses.
  if (frame != 60 && frame % kReportInterval != 0)
    return;

  const uint64_t targets = g_totalTargets.load(std::memory_order_relaxed);
  size_t distinctShapes = 0;
  {
    std::lock_guard lock(g_shapesMutex);
    distinctShapes = g_shapes.size();
  }
  UINT swapWidth = 0, swapHeight = 0;
  UINT mainWidth = 0, mainHeight = 0;
  unpackSize(g_swapSize.load(std::memory_order_acquire),
             &swapWidth, &swapHeight);
  unpackSize(g_mainSize.load(std::memory_order_acquire),
             &mainWidth, &mainHeight);
  log("TARGETCENSUS frame=", std::dec, frame,
      " swapChain=", swapWidth, "x", swapHeight,
      " mainRT=", mainWidth, "x", mainHeight,
      " targetsCreated=", targets,
      " distinctShapes=", distinctShapes,
      " rsViewports=", g_rsViewportCalls.load(std::memory_order_relaxed),
      " rsScissors=", g_rsScissorCalls.load(std::memory_order_relaxed),
      " draws=", g_drawCalls.load(std::memory_order_relaxed),
      " updates=", g_updatesEntered.load(std::memory_order_relaxed),
      " drawsTo1x=", g_drawsSingleSample.load(std::memory_order_relaxed),
      " drawsToMsaa=", g_drawsMultiSample.load(std::memory_order_relaxed),
      " maxBoundSamples=", g_maxBoundSamples.load(std::memory_order_relaxed),
      " targetLookupFails=",
        g_targetLookupFails.load(std::memory_order_relaxed),
      " viewportRewrites=", g_viewportRewrites.load(std::memory_order_relaxed),
      " scissorRewrites=", g_scissorRewrites.load(std::memory_order_relaxed),
      g_shapesOverflowed ? " (shape table capped; totals remain exact)" : "");
}

}  // namespace atfix
