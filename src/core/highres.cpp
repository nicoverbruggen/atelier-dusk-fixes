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
// Vtable slots, verified the same way the other hooks in this tree are:
//
//   IUnknown             : QueryInterface, AddRef, Release            -- 0-2
//   ID3D11Device         : its own methods in declaration order, from 3
//   ID3D11DeviceChild    : GetDevice, Get/SetPrivateData,
//                          SetPrivateDataInterface                    -- 3-6
//   ID3D11DeviceContext  : its own methods in declaration order, from 7
//
// ID3D11Device's first four are CreateBuffer, CreateTexture1D,
// CreateTexture2D, CreateTexture3D, so CreateTexture2D is slot 5 (3+2).
//
// On the context, counting from VSSetConstantBuffers (7), RSSetViewports is the
// 38th of its own methods (7+37 = 44) and RSSetScissorRects the 39th (45).
// These are consistent with the numbers already verified in d3d11_probe.cpp
// against the same header: Map at 14, CopySubresourceRegion/CopyResource/
// UpdateSubresource at 46/47/48.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "game.h"
#include "highres.h"
#include "log.h"
#include "util.h"
#include "../../vendor/minhook/include/MinHook.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// Matches the cadence of the other periodic diagnostics (d3d11_probe.cpp's
// kReportInterval), so two of them running together interleave predictably.
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

// The main render size, learned from the first depth-stencil target the game
// creates. Atomics because nothing guarantees the engine creates resources and
// submits raster state on the same thread, and the cost is irrelevant next to a
// texture creation.
std::atomic<UINT> g_mainWidth{0};
std::atomic<UINT> g_mainHeight{0};
std::atomic<UINT> g_swapWidth{0};
std::atomic<UINT> g_swapHeight{0};

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

  const UINT swapWidth = g_swapWidth.load(std::memory_order_relaxed);
  const UINT swapHeight = g_swapHeight.load(std::memory_order_relaxed);
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
  const UINT swapWidth = g_swapWidth.load(std::memory_order_relaxed);
  const UINT swapHeight = g_swapHeight.load(std::memory_order_relaxed);
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
// BGRA, no mips, no array, no MSAA -- because "half of the pinned size" is a
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

using PFN_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *) (
  ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
  ID3D11Texture2D**);

PFN_CreateTexture2D originalCreateTexture2D = nullptr;

HRESULT STDMETHODCALLTYPE hookedCreateTexture2D(
    ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Texture2D** texture) {
  if (!desc)
    return originalCreateTexture2D(self, desc, initialData, texture);

  D3D11_TEXTURE2D_DESC local = *desc;
  const char* action = "passthrough";

  if (g_fixEnabled) {
    if (!g_mainWidth.load(std::memory_order_relaxed) &&
        looksLikeMainTarget(local)) {
      g_mainWidth.store(local.Width, std::memory_order_relaxed);
      g_mainHeight.store(local.Height, std::memory_order_relaxed);
      log("HIGHRES: main render size ", std::dec, local.Width, "x",
          local.Height);
      action = "adoptedAsMain";
    } else {
      const UINT mainWidth = g_mainWidth.load(std::memory_order_relaxed);
      const UINT mainHeight = g_mainHeight.load(std::memory_order_relaxed);
      // Only ever scales up. At or below the pinned size there is nothing to
      // correct, and rewriting anyway would mean an ordinary 1080p session
      // taking a different path from the one the game shipped with.
      if (mainWidth > kPinnedWidth && mainHeight > kPinnedHeight) {
        if (isPinnedFullTarget(local, initialData)) {
          local.Width = mainWidth;
          local.Height = mainHeight;
          action = "resizedFull";
        } else if (isPinnedBlurTarget(local, initialData)) {
          local.Width = mainWidth / 2;
          local.Height = mainHeight / 2;
          action = "resizedBlur";
        }
      }
    }
  }

  // Reported before forwarding and from the descriptor the game asked for, so
  // the census keeps saying what the game wanted; `action` says what it got.
  if (g_censusEnabled && isTarget(*desc))
    censusReport(*desc, censusCallerRva(), action);

  const HRESULT hr =
    originalCreateTexture2D(self, &local, initialData, texture);
  // A resize the driver refuses is not survivable silently: the game would get
  // no texture at all where it expected one. Retry with exactly what it asked
  // for, so the worst case is the unfixed 1080p frame rather than a broken one.
  if (FAILED(hr) && (local.Width != desc->Width || local.Height != desc->Height)) {
    log("HIGHRES: resize to ", std::dec, local.Width, "x", local.Height,
        " was refused (hr=0x", std::hex, uint32_t(hr), std::dec,
        "); falling back to the game's own ", desc->Width, "x", desc->Height);
    return originalCreateTexture2D(self, desc, initialData, texture);
  }
  return hr;
}

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
struct RasterState {
  std::atomic<bool> dirty{false};
};

// Two states, as in the Arland implementation: the immediate context and
// everything else (the engine's deferred contexts). Keyed by comparing against
// the immediate context recorded at install, so the hot path is a pointer
// compare rather than a map lookup under a lock.
RasterState g_immediateRaster;
RasterState g_deferredRaster;
ID3D11DeviceContext* g_immediateContext = nullptr;

RasterState& rasterStateFor(ID3D11DeviceContext* context) {
  return context == g_immediateContext ? g_immediateRaster : g_deferredRaster;
}

// Forward declaration: the detours below are referenced by the install table.
void STDMETHODCALLTYPE hookedRSSetViewports(
  ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
void STDMETHODCALLTYPE hookedRSSetScissorRects(
  ID3D11DeviceContext*, UINT, const D3D11_RECT*);

using PFN_RSSetViewports = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN_RSSetScissorRects = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using PFN_Draw = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexed = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_DrawInstanced = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

// The originals, one set per context vtable.
//
// This is the part the first two attempts got wrong, and it cost two runs to
// find. MinHook hooks a function ADDRESS, and an address comes from a vtable --
// so hooking the immediate context's vtable hooks only contexts of that class.
// D3D11 implementations give deferred contexts a different class with a
// different vtable, and this engine issues its draws and its raster state on a
// DEFERRED context: an instrumented 1080p run recorded rsViewports=0 and
// draws=0 across 1500 frames with all six hooks installed and reporting
// success, while the device-level CreateTexture2D hook on the same run fired
// normally.
//
// (That also explains why d3d11_probe.cpp's Map hook has always worked on the
// immediate context: texture uploads go through it. Drawing does not.)
//
// So both vtables are hooked, and each detour dispatches to the originals
// belonging to the vtable its context actually came from.
struct ContextOriginals {
  PFN_RSSetViewports rsSetViewports = nullptr;
  PFN_RSSetScissorRects rsSetScissorRects = nullptr;
  PFN_Draw draw = nullptr;
  PFN_DrawIndexed drawIndexed = nullptr;
  PFN_DrawInstanced drawInstanced = nullptr;
  PFN_DrawIndexedInstanced drawIndexedInstanced = nullptr;
};

ContextOriginals g_immediateOriginals;
ContextOriginals g_deferredOriginals;
void** g_immediateVtable = nullptr;

// Which set to forward to, decided from the context's own vtable pointer rather
// than from the object identity: the engine may hold several deferred contexts
// and they all share one vtable.
const ContextOriginals& originalsFor(ID3D11DeviceContext* context) {
  return *reinterpret_cast<void***>(context) == g_immediateVtable
    ? g_immediateOriginals : g_deferredOriginals;
}

// The size of whatever is bound as render target 0, or depth-stencil if there
// is no colour target. Nothing is assumed about which of the two is present.
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
  RasterState& state = rasterStateFor(context);
  if (!state.dirty.exchange(false, std::memory_order_acq_rel))
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

  const ContextOriginals& originals = originalsFor(context);
  if (rewroteViewport && originals.rsSetViewports) {
    originals.rsSetViewports(context, 1, &viewport);
    g_viewportRewrites.fetch_add(1, std::memory_order_relaxed);
  }
  if (rewroteScissor && originals.rsSetScissorRects) {
    originals.rsSetScissorRects(context, 1, &scissor);
    g_scissorRewrites.fetch_add(1, std::memory_order_relaxed);
  }
}

void STDMETHODCALLTYPE hookedRSSetViewports(
    ID3D11DeviceContext* self, UINT count, const D3D11_VIEWPORT* viewports) {
  originalsFor(self).rsSetViewports(self, count, viewports);
  g_rsViewportCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    rasterStateFor(self).dirty.store(true, std::memory_order_release);
}

void STDMETHODCALLTYPE hookedRSSetScissorRects(
    ID3D11DeviceContext* self, UINT count, const D3D11_RECT* rects) {
  originalsFor(self).rsSetScissorRects(self, count, rects);
  g_rsScissorCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    rasterStateFor(self).dirty.store(true, std::memory_order_release);
}

// The four draw entry points. Each does the same thing: settle any pending
// raster state, then forward. The dirty flag means the work above runs at most
// once per raster change rather than once per draw.
void STDMETHODCALLTYPE hookedDraw(
    ID3D11DeviceContext* self, UINT vertexCount, UINT startVertex) {
  g_drawCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    updateViewportScissor(self);
  originalsFor(self).draw(self, vertexCount, startVertex);
}

void STDMETHODCALLTYPE hookedDrawIndexed(
    ID3D11DeviceContext* self, UINT indexCount, UINT startIndex,
    INT baseVertex) {
  g_drawCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    updateViewportScissor(self);
  originalsFor(self).drawIndexed(self, indexCount, startIndex, baseVertex);
}

void STDMETHODCALLTYPE hookedDrawInstanced(
    ID3D11DeviceContext* self, UINT vertexCountPerInstance, UINT instanceCount,
    UINT startVertex, UINT startInstance) {
  g_drawCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    updateViewportScissor(self);
  originalsFor(self).drawInstanced(self, vertexCountPerInstance, instanceCount,
                                   startVertex, startInstance);
}

void STDMETHODCALLTYPE hookedDrawIndexedInstanced(
    ID3D11DeviceContext* self, UINT indexCountPerInstance, UINT instanceCount,
    UINT startIndex, INT baseVertex, UINT startInstance) {
  g_drawCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_fixEnabled)
    updateViewportScissor(self);
  originalsFor(self).drawIndexedInstanced(self, indexCountPerInstance,
                                          instanceCount, startIndex,
                                          baseVertex, startInstance);
}

// Same ordering discipline as the other installers in this tree: every
// MH_CreateHook is attempted before any MH_EnableHook, so a failure partway
// through leaves every target un-enabled (pass-through) rather than some hooks
// live and others not. That matters more here than anywhere else -- a live
// CreateTexture2D hook with a dead raster correction resizes the targets and
// leaves the viewport behind, which is a visibly broken frame rather than an
// unfixed one.
//
// The first version of this could reach exactly that state silently: the
// context hooks were skipped when no context was available, and it still
// logged a flat "installed fix=1". The log now names every hook, and a fix
// that cannot install its raster correction declines to install at all rather
// than half-applying itself.
bool installHooks(ID3D11Device* device, ID3D11DeviceContext* context) {
  // The Phyre module initializes MinHook for Ayesha, but this subsystem is the
  // only thing in the tree that hooks anything on Escha & Logy or Shallie, so
  // it cannot assume someone else has. MinHook answers a second call with
  // MH_ERROR_ALREADY_INITIALIZED, which is a success for our purposes.
  const MH_STATUS init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
    log("HIGHRES: MH_Initialize failed (", MH_StatusToString(init),
        "), installing nothing");
    return false;
  }

  auto** deviceVtable = *reinterpret_cast<void***>(device);
  void* createTarget = deviceVtable[5];

  // Context vtable slots, counted from the MinGW/Wine d3d11.h this cross-build
  // uses: RSSetViewports and RSSetScissorRects are the 45th and 46th entries
  // (44 and 45 zero-based), and the four draws are Draw 13, DrawIndexed 12,
  // DrawInstanced 21, DrawIndexedInstanced 20. The same table puts Map at 14
  // and CopyResource at 47, which is what d3d11_probe.cpp already hooks
  // successfully -- so the numbering is confirmed against working code, not
  // just read off a header.
  // Context vtable slots, counted from the MinGW/Wine d3d11.h this cross-build
  // uses: RSSetViewports and RSSetScissorRects are the 45th and 46th entries
  // (44 and 45 zero-based), DrawIndexed 12, Draw 13, DrawIndexedInstanced 20,
  // DrawInstanced 21. The same table puts Map at 14, which is what
  // d3d11_probe.cpp already hooks successfully, so the numbering is confirmed
  // against working code rather than only read off a header.
  struct HookSpec {
    int slot;
    void* detour;
    size_t originalOffset;   // into ContextOriginals
    const char* name;
  };
  #define ORIGINAL_AT(member) offsetof(ContextOriginals, member)
  const HookSpec contextHooks[] = {
    { 44, reinterpret_cast<void*>(&hookedRSSetViewports),
      ORIGINAL_AT(rsSetViewports), "RSSetViewports" },
    { 45, reinterpret_cast<void*>(&hookedRSSetScissorRects),
      ORIGINAL_AT(rsSetScissorRects), "RSSetScissorRects" },
    { 12, reinterpret_cast<void*>(&hookedDrawIndexed),
      ORIGINAL_AT(drawIndexed), "DrawIndexed" },
    { 13, reinterpret_cast<void*>(&hookedDraw),
      ORIGINAL_AT(draw), "Draw" },
    { 20, reinterpret_cast<void*>(&hookedDrawIndexedInstanced),
      ORIGINAL_AT(drawIndexedInstanced), "DrawIndexedInstanced" },
    { 21, reinterpret_cast<void*>(&hookedDrawInstanced),
      ORIGINAL_AT(drawInstanced), "DrawInstanced" },
  };
  #undef ORIGINAL_AT
  constexpr int kContextHookCount =
    int(sizeof(contextHooks) / sizeof(contextHooks[0]));

  // The census alone needs no raster correction, so the context hooks go in
  // only when the fix is on. A diagnostic that changed raster state would not
  // be a diagnostic.
  const bool wantContextHooks = g_fixEnabled;
  if (wantContextHooks && !context) {
    log("HIGHRES: no immediate context available, so the raster correction"
        " cannot be installed; declining to resize targets without it");
    return false;
  }

  // Hook one context's vtable, filling `originals` with its trampolines.
  // Enabling is done here rather than in a second pass because the two vtables
  // are independent: a deferred context that cannot be hooked is a reason to
  // decline, and the caller undoes the immediate set in that case.
  auto hookContextVtable =
      [&](ID3D11DeviceContext* target, ContextOriginals& originals,
          const char* which) -> bool {
    auto** vtable = *reinterpret_cast<void***>(target);
    auto* base = reinterpret_cast<uint8_t*>(&originals);
    for (int i = 0; i < kContextHookCount; ++i) {
      void* fn = vtable[contextHooks[i].slot];
      void** slot = reinterpret_cast<void**>(base + contextHooks[i].originalOffset);
      const MH_STATUS created =
        MH_CreateHook(fn, contextHooks[i].detour, slot);
      // The two vtables can legitimately share an entry -- an implementation is
      // free to give both context types the same function for a method that
      // does not differ. MinHook refuses the second hook on that address, which
      // is not an error: the first install already covers it. Reuse the
      // trampoline it produced, which is the one recorded in the immediate
      // set, since that vtable is always hooked first.
      if (created == MH_ERROR_ALREADY_CREATED) {
        auto* immediateBase = reinterpret_cast<uint8_t*>(&g_immediateOriginals);
        *slot = *reinterpret_cast<void**>(
          immediateBase + contextHooks[i].originalOffset);
        continue;
      }
      if (created != MH_OK) {
        log("HIGHRES: MH_CreateHook(", which, "::", contextHooks[i].name,
            ") failed: ", MH_StatusToString(created));
        return false;
      }
      if (MH_EnableHook(fn) != MH_OK) {
        log("HIGHRES: MH_EnableHook(", which, "::", contextHooks[i].name,
            ") failed");
        return false;
      }
    }
    return true;
  };

  if (MH_CreateHook(createTarget, reinterpret_cast<void*>(&hookedCreateTexture2D),
                    reinterpret_cast<void**>(&originalCreateTexture2D)) != MH_OK ||
      MH_EnableHook(createTarget) != MH_OK) {
    log("HIGHRES: could not hook CreateTexture2D, installing nothing");
    return false;
  }

  int hookedVtables = 0;
  if (wantContextHooks) {
    g_immediateVtable = *reinterpret_cast<void***>(context);
    if (!hookContextVtable(context, g_immediateOriginals, "immediate")) {
      MH_DisableHook(createTarget);
      return false;
    }
    ++hookedVtables;

    // And the deferred context's vtable, which is where this engine actually
    // draws. One is created purely to read its vtable and then released; every
    // deferred context the game makes shares that vtable, so hooking through
    // ours covers all of them.
    ID3D11DeviceContext* deferred = nullptr;
    const HRESULT hr = device->CreateDeferredContext(0, &deferred);
    if (FAILED(hr) || !deferred) {
      log("HIGHRES: CreateDeferredContext failed (hr=0x", std::hex,
          uint32_t(hr), std::dec, "); the raster correction would miss every"
          " draw this engine issues, so installing nothing");
      MH_DisableHook(createTarget);
      return false;
    }
    void** deferredVtable = *reinterpret_cast<void***>(deferred);
    const bool distinct = deferredVtable != g_immediateVtable;
    log("HIGHRES: context vtables immediate=",
        reinterpret_cast<void*>(g_immediateVtable),
        " deferred=", reinterpret_cast<void*>(deferredVtable),
        distinct ? " (distinct, both hooked)" : " (shared, one hook set)");
    bool ok = true;
    if (distinct) {
      ok = hookContextVtable(deferred, g_deferredOriginals, "deferred");
      if (ok)
        ++hookedVtables;
    } else {
      // One vtable serves both; the immediate set already covers it.
      g_deferredOriginals = g_immediateOriginals;
    }
    deferred->Release();
    if (!ok) {
      MH_DisableHook(createTarget);
      return false;
    }
  }

  g_immediateContext = context;
  log("HIGHRES: installed fix=", g_fixEnabled ? 1 : 0,
      " census=", g_censusEnabled ? 1 : 0,
      " contextVtables=", hookedVtables,
      " hooksPerVtable=", wantContextHooks ? kContextHookCount : 0);
  return true;
}

}  // namespace

void initializeHighRes(ID3D11Device* device, ID3D11DeviceContext* context) {
  static bool done = false;
  if (done || !device)
    return;
  g_fixEnabled = featureEnabled(Feature::HighResRendering);
  g_censusEnabled = featureEnabled(Feature::TargetCensus);
  if (!g_fixEnabled && !g_censusEnabled)
    return;
  // If no context came with the device, ask the device for its own rather than
  // installing a half-fix. This is the case the first version got wrong: the
  // raster correction was quietly skipped and only the resize went in.
  ID3D11DeviceContext* owned = nullptr;
  if (!context) {
    device->GetImmediateContext(&owned);
    context = owned;
  }
  g_installed = installHooks(device, context);
  if (owned)
    owned->Release();
  // Latched only on success, matching main.cpp's hookPresent: a failed attempt
  // leaves a later device free to try again rather than doing nothing for the
  // rest of the session.
  done = g_installed;
}

void noteSwapChainSize(unsigned int width, unsigned int height,
                       unsigned int format, unsigned int refreshNumerator,
                       unsigned int refreshDenominator, bool windowed) {
  g_swapWidth.store(width, std::memory_order_relaxed);
  g_swapHeight.store(height, std::memory_order_relaxed);
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
  log("TARGETCENSUS frame=", std::dec, frame,
      " swapChain=", g_swapWidth.load(std::memory_order_relaxed), "x",
      g_swapHeight.load(std::memory_order_relaxed),
      " mainRT=", g_mainWidth.load(std::memory_order_relaxed), "x",
      g_mainHeight.load(std::memory_order_relaxed),
      " targetsCreated=", targets,
      " distinctShapes=", distinctShapes,
      " rsViewports=", g_rsViewportCalls.load(std::memory_order_relaxed),
      " rsScissors=", g_rsScissorCalls.load(std::memory_order_relaxed),
      " draws=", g_drawCalls.load(std::memory_order_relaxed),
      " updates=", g_updatesEntered.load(std::memory_order_relaxed),
      " targetLookupFails=",
        g_targetLookupFails.load(std::memory_order_relaxed),
      " viewportRewrites=", g_viewportRewrites.load(std::memory_order_relaxed),
      " scissorRewrites=", g_scissorRewrites.load(std::memory_order_relaxed),
      g_shapesOverflowed ? " (shape table capped; totals remain exact)" : "");
}

}  // namespace atfix
