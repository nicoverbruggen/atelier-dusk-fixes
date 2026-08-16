// SPDX-License-Identifier: MIT
//
// See shadow_res.h for what this fixes and why it takes this shape. What is
// here is the twin bookkeeping and the notes that only mean anything beside the
// code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <unordered_set>

#include "shadow_res.h"
#include "config.h"
#include "d3d11_hooks.h"
#include "game.h"
#include "highres.h"
#include "log.h"
#include "util.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// Private-data keys. The twin hangs off its host by SetPrivateDataInterface, so
// the host owning it is what gives the twin the host's exact lifetime: no
// tracking table, and nothing to clean up when the engine releases a map.
const GUID IID_ShadowResTwin =
  {0x7c1f4a20,0x5b83,0x4d6e,{0x9a,0x11,0x2e,0x74,0xc8,0x0d,0x51,0x33}};
const GUID IID_ShadowResTwinView =
  {0x7c1f4a21,0x5b83,0x4d6e,{0x9a,0x11,0x2e,0x74,0xc8,0x0d,0x51,0x33}};

// Views that are known NOT to sit over a twin. Every PSSetShaderResources call
// on this engine passes several views and almost none of them is the shadow
// map, so without this the substitution walks the private data of every texture
// the receiver samples, every draw.
//
// Cleared whenever a twin is created, because a scene rebuild can recycle a
// view pointer and a stale negative would suppress the redirect for good.
mutex g_negativeMutex;
std::unordered_set<uintptr_t> g_negativeViews;

std::atomic<bool> g_anyTwin{false};

bool isShadowMapDescriptor(const D3D11_TEXTURE2D_DESC& desc) {
  // Size, format and sample count identify it; the census settles that these
  // three separate it from every other shape Ayesha creates. The rest are
  // safety conditions, so anything unexpected declines rather than being
  // guessed at.
  return desc.Width == 1024 && desc.Height == 1024 &&
         desc.Format == DXGI_FORMAT_R24G8_TYPELESS &&
         desc.SampleDesc.Count == 1 &&
         desc.Usage == D3D11_USAGE_DEFAULT &&
         desc.CPUAccessFlags == 0 && desc.MiscFlags == 0 &&
         desc.MipLevels == 1 && desc.ArraySize == 1;
}

// The twin resource behind a host, RETAINED. GetPrivateData calls AddRef when
// the data was set by SetPrivateDataInterface, so there is no borrow available
// here however convenient one would be, and every caller releases.
ID3D11Resource* twinOf(ID3D11Resource* host) {
  if (!host || !g_anyTwin.load(std::memory_order_relaxed))
    return nullptr;
  ID3D11Resource* twin = nullptr;
  UINT size = sizeof(twin);
  if (SUCCEEDED(host->GetPrivateData(IID_ShadowResTwin, &size, &twin)) && twin)
    return twin;
  return nullptr;
}

// A view over the twin of whatever `hostView` sits on, cached on the host view
// itself so the second and later binds cost one GetPrivateData. `create` builds
// the view; the two callers differ only in which D3D entry point that is.
template <typename View, typename Create>
View* twinViewOf(View* hostView, Create create) {
  View* twinView = nullptr;
  UINT size = sizeof(twinView);
  if (SUCCEEDED(hostView->GetPrivateData(IID_ShadowResTwinView, &size,
                                         &twinView)) && twinView)
    return twinView;

  ID3D11Resource* hostRes = nullptr;
  hostView->GetResource(&hostRes);
  ID3D11Resource* twinRes = twinOf(hostRes);
  if (hostRes)
    hostRes->Release();
  if (!twinRes)
    return nullptr;

  ID3D11Device* device = nullptr;
  hostView->GetDevice(&device);
  HRESULT hr = E_FAIL;
  if (device) {
    hr = create(hostView, device, twinRes, &twinView);
    device->Release();
  }
  if (FAILED(hr) || !twinView) {
    twinRes->Release();
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
      log("SHADOWRES twin view creation FAILED hr=0x", std::hex, hr,
          std::dec, "; this map keeps the vanilla path");
    return nullptr;
  }
  twinRes->Release();
  hostView->SetPrivateDataInterface(IID_ShadowResTwinView, twinView);
  return twinView;   // the creation reference belongs to the caller
}

// The twin DSV for a host DSV, built from the engine's OWN view descriptor.
// Constructing one instead is a guess: it creates successfully either way, so a
// mismatch shows up as a picture rather than an error.
ID3D11DepthStencilView* twinDsvFor(ID3D11DepthStencilView* hostDsv) {
  return twinViewOf(hostDsv, [](ID3D11DepthStencilView* host,
                                ID3D11Device* device, ID3D11Resource* res,
                                ID3D11DepthStencilView** out) {
    D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
    host->GetDesc(&desc);
    return device->CreateDepthStencilView(res, &desc, out);
  });
}

}  // namespace

unsigned int shadowMapResolution() {
  static const unsigned int resolution = [] () -> unsigned int {
    // 2 when the key is missing, matching what the launcher writes and the
    // Arland project. A player who deletes the key gets the feature, not the
    // vanilla path.
    const int multiplier = duskConfigInt("Rendering", "ShadowMultiplier", 2);
    if (multiplier == 2 || multiplier == 4 || multiplier == 8)
      return 1024u * unsigned(multiplier);
    return 1024u;   // any other value is the vanilla path, byte for byte
  }();
  return resolution;
}

bool shadowResWanted() {
  static const bool wanted = [] {
    // The matrix cell says which games have this at all; it does NOT say
    // whether it is on. This is a valued knob, so the ini value is the switch
    // and 1 is off -- asking featureEnabled here would answer for a boolean
    // the descriptor deliberately does not carry.
    if (featureSupport(Feature::ShadowMultiplier) == Support::Unsupported)
      return false;
    if (shadowMapResolution() <= 1024)
      return false;
    // The enlarged caster pass depends on the raster correction to carry its
    // viewport. Without it the casters would draw into one corner of the twin
    // and the receiver would sample undefined depth everywhere else.
    if (!featureEnabled(Feature::HighResRendering)) {
      log("FIXES shadow_resolution=declined: it needs the high-resolution fix"
          " to carry the caster viewport, and that is off this session");
      return false;
    }
    return true;
  }();
  return wanted;
}

void shadowResNoteCreation(ID3D11Device* device,
                           const D3D11_TEXTURE2D_DESC* originalDesc,
                           const D3D11_SUBRESOURCE_DATA* initialData,
                           ID3D11Texture2D* created) {
  if (!shadowResWanted() || !device || !originalDesc || !created)
    return;
  // Initial data would have to be resampled to fill the larger map, and a
  // shadow map never arrives with any. Declining is the honest answer.
  if (initialData || !isShadowMapDescriptor(*originalDesc))
    return;

  const unsigned int size = shadowMapResolution();
  D3D11_TEXTURE2D_DESC twinDesc = *originalDesc;
  twinDesc.Width = size;
  twinDesc.Height = size;

  // Through the unhooked entry point, so the mod's own texture cannot re-enter
  // the resolution fix's creation detour and be classified as a game target.
  ID3D11Texture2D* twin = nullptr;
  const HRESULT hr = createTexture2DUnhooked(device, &twinDesc, nullptr, &twin);
  if (FAILED(hr) || !twin) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
      log("SHADOWRES twin creation FAILED hr=0x", std::hex, hr, std::dec,
          "; this map keeps the vanilla ", originalDesc->Width, "x",
          originalDesc->Height, " path");
    return;
  }

  created->SetPrivateDataInterface(IID_ShadowResTwin, twin);
  twin->Release();   // the host's private data keeps it alive from here
  g_anyTwin.store(true, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_negativeMutex);
    g_negativeViews.clear();   // a new generation: re-probe every view
  }

  static std::atomic<bool> announced{false};
  if (!announced.exchange(true, std::memory_order_relaxed))
    log("FIXES shadow_resolution=active size=", std::dec, size, "x", size,
        " (the engine's own ", originalDesc->Width, "x", originalDesc->Height,
        " map is left untouched)");
  // Every twin, not just the first. How MANY maps this engine allocates is the
  // question that decides whether the caster and the receiver share one texture
  // or need the copy below to join them, and a once-only announcement cannot
  // answer it.
  if (verboseLogging())
    log("SHADOWRES twin created ", std::dec, size, "x", size, " for host=",
        created, " bind=0x", std::hex, originalDesc->BindFlags, std::dec);
}

void shadowResMirrorClear(ID3D11DeviceContext* context,
                          ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth,
                          UINT8 stencil) {
  if (!shadowResWanted() || !context || !dsv ||
      !g_anyTwin.load(std::memory_order_relaxed))
    return;
  ID3D11DepthStencilView* twinDsv = twinDsvFor(dsv);
  if (!twinDsv)
    return;
  // DUSK_SHADOW_PROBE inverts the clear as a diagnostic: depth 0 is nearest, so
  // every receiver comparison should find the scene behind the shadow map and
  // the whole picture should go shadowed. If it does, the receiver is genuinely
  // sampling this texture and the caster is what is not writing to it. If the
  // picture is unchanged, the receiver is not reading this texture at all,
  // whatever the substitution counter says. Diagnostic only, never shipped on.
  static const bool probe = [] {
    const char* v = std::getenv("DUSK_SHADOW_PROBE");
    return v && v[0] != '0';
  }();
  const FLOAT used = probe ? 0.0f : depth;
  d3d11OriginalsFor(context).clearDepthStencilView(context, twinDsv, flags,
                                                   used, stencil);
  twinDsv->Release();
  static std::atomic<uint32_t> cleared{0};
  const uint32_t n = cleared.fetch_add(1, std::memory_order_relaxed);
  if (n == 0 || (verboseLogging() && n % 4096 == 0))
    log("SHADOWRES twin cleared to depth=", used, " (engine asked ", depth,
        ") flags=0x", std::hex, flags, std::dec, " probe=", probe ? 1 : 0,
        " (n=", n + 1, ")");
}

ID3D11DepthStencilView* shadowResRedirectDsv(
    ID3D11DepthStencilView* dsv, UINT rtvCount,
    ID3D11RenderTargetView* const* rtvs) {
  if (!shadowResWanted() || !dsv || !g_anyTwin.load(std::memory_order_relaxed))
    return nullptr;
  ID3D11DepthStencilView* twinDsv = twinDsvFor(dsv);
  if (!twinDsv)
    return nullptr;

  // Depth-only, or nothing. A colour target bound alongside is still the
  // engine's own size, and binding a 2048 depth beside a 1024 colour is a
  // mismatch D3D would reject and a picture nobody wants. Never observed on
  // this engine, which is why it declines loudly rather than silently.
  if (rtvCount != 0 && rtvs && rtvs[0]) {
    static std::atomic<uint32_t> skipped{0};
    const uint32_t n = skipped.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || (verboseLogging() && n % 4096 == 0))
      log("SHADOWRES caster redirect declined: a colour target is bound with"
          " the shadow map, so the vanilla pass runs (n=", std::dec, n + 1, ")");
    twinDsv->Release();
    return nullptr;
  }

  static std::atomic<uint32_t> redirects{0};
  const uint32_t n = redirects.fetch_add(1, std::memory_order_relaxed);
  if (verboseLogging() && (n < 4 || n % 4096 == 0))
    log("SHADOWRES caster bound to the twin (n=", std::dec, n + 1, ")");
  return twinDsv;
}

bool shadowResSubstituteSrvs(UINT numViews,
                             ID3D11ShaderResourceView* const* views,
                             ID3D11ShaderResourceView** out, UINT outCapacity) {
  if (!shadowResWanted() || !numViews || !views || !out ||
      numViews > outCapacity || !g_anyTwin.load(std::memory_order_relaxed))
    return false;

  bool any = false;
  for (UINT i = 0; i < numViews; ++i) {
    out[i] = views[i];
    if (!out[i])
      continue;
    {
      std::lock_guard lock(g_negativeMutex);
      if (g_negativeViews.count(reinterpret_cast<uintptr_t>(out[i])))
        continue;
    }
    ID3D11ShaderResourceView* twinSrv = twinViewOf(
      out[i], [](ID3D11ShaderResourceView* host, ID3D11Device* device,
                 ID3D11Resource* res, ID3D11ShaderResourceView** result) {
        // The engine's own descriptor, not one of ours. It knows the format
        // and mip range its comparison sampler expects; a constructed
        // descriptor is a guess that happens to create successfully.
        D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
        host->GetDesc(&desc);
        return device->CreateShaderResourceView(res, &desc, result);
      });
    if (twinSrv) {
      out[i] = twinSrv;   // retained; the caller releases after forwarding
      any = true;
    } else {
      std::lock_guard lock(g_negativeMutex);
      g_negativeViews.insert(reinterpret_cast<uintptr_t>(views[i]));
    }
  }
  if (any) {
    static std::atomic<uint32_t> redirects{0};
    const uint32_t n = redirects.fetch_add(1, std::memory_order_relaxed);
    if (verboseLogging() && (n < 4 || n % 4096 == 0))
      log("SHADOWRES receiver sampling the twin (n=", std::dec, n + 1, ")");
  }
  return any;
}

void shadowResApplyCasterViewport(ID3D11DeviceContext* context) {
  if (!shadowResWanted() || !context)
    return;
  const float size = float(shadowMapResolution());
  D3D11_VIEWPORT viewport = {};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = size;
  viewport.Height = size;
  // The engine's own depth range, not a guess: a caster pass that remaps depth
  // would be broken by 0..1 substituted underneath it.
  UINT count = 1;
  D3D11_VIEWPORT current = {};
  context->RSGetViewports(&count, &current);
  viewport.MinDepth = count == 1 ? current.MinDepth : 0.0f;
  viewport.MaxDepth = count == 1 ? current.MaxDepth : 1.0f;

  D3D11_RECT scissor = {};
  scissor.left = 0;
  scissor.top = 0;
  scissor.right = LONG(shadowMapResolution());
  scissor.bottom = LONG(shadowMapResolution());

  const ContextOriginals& originals = d3d11OriginalsFor(context);
  if (originals.rsSetViewports)
    originals.rsSetViewports(context, 1, &viewport);
  if (originals.rsSetScissorRects)
    originals.rsSetScissorRects(context, 1, &scissor);

  static std::atomic<uint32_t> applied{0};
  const uint32_t n = applied.fetch_add(1, std::memory_order_relaxed);
  if (n == 0 || (verboseLogging() && n % 4096 == 0))
    log("SHADOWRES caster raster set to ", std::dec, UINT(size), "x",
        UINT(size), " depth ", viewport.MinDepth, "..", viewport.MaxDepth,
        " (n=", n + 1, ")");
}

namespace {

// Mirror one shadow-map-to-shadow-map transfer onto the twins. Both twins are
// the same size, so a whole-subresource copy is always legal; translating the
// engine's box into twin coordinates would be a guess about a rectangle stated
// in 1024 space, and the transfer this exists for copies the whole map anyway.
//
// If either side has no twin this does nothing and the engine's own copy is the
// only one that happens, which is the vanilla path exactly.
void mirrorCopyOnTwins(ID3D11DeviceContext* context, ID3D11Resource* dst,
                       ID3D11Resource* src, const char* what) {
  if (!shadowResWanted() || !context || !dst || !src ||
      !g_anyTwin.load(std::memory_order_relaxed))
    return;
  ID3D11Resource* dstTwin = twinOf(dst);
  if (!dstTwin)
    return;
  ID3D11Resource* srcTwin = twinOf(src);
  if (srcTwin) {
    d3d11OriginalsFor(context).copySubresourceRegion(context, dstTwin, 0, 0, 0,
                                                     0, srcTwin, 0, nullptr);
    srcTwin->Release();
    static std::atomic<uint32_t> mirrored{0};
    const uint32_t n = mirrored.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || (verboseLogging() && n % 4096 == 0))
      log("SHADOWRES mirrored ", what, " onto the twins (n=", std::dec, n + 1,
          ")");
  } else {
    // A twinned destination fed from an untwinned source would leave the twin
    // holding stale caster depth while the engine's own map moved on, which
    // reads in game as shadows from a previous frame rather than none.
    static std::atomic<uint32_t> halved{0};
    const uint32_t n = halved.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || (verboseLogging() && n % 4096 == 0))
      log("SHADOWRES ", what, " into a twinned map from a source with no twin;"
          " that twin now holds stale depth (n=", std::dec, n + 1, ")");
  }
  dstTwin->Release();
}

}  // namespace

void STDMETHODCALLTYPE hookedCopyResource(ID3D11DeviceContext* self,
                                          ID3D11Resource* dst,
                                          ID3D11Resource* src) {
  d3d11OriginalsFor(self).copyResource(self, dst, src);
  // After the engine's own copy, so a failure here leaves the vanilla transfer
  // done and the engine's 1024 map correct.
  mirrorCopyOnTwins(self, dst, src, "CopyResource");
}

void STDMETHODCALLTYPE hookedCopySubresourceRegion(
    ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSubresource,
    UINT dstX, UINT dstY, UINT dstZ, ID3D11Resource* src, UINT srcSubresource,
    const D3D11_BOX* srcBox) {
  d3d11OriginalsFor(self).copySubresourceRegion(self, dst, dstSubresource, dstX,
                                                dstY, dstZ, src, srcSubresource,
                                                srcBox);
  mirrorCopyOnTwins(self, dst, src, "CopySubresourceRegion");
}

void STDMETHODCALLTYPE hookedClearDepthStencilView(
    ID3D11DeviceContext* self, ID3D11DepthStencilView* dsv, UINT flags,
    FLOAT depth, UINT8 stencil) {
  d3d11OriginalsFor(self).clearDepthStencilView(self, dsv, flags, depth,
                                                stencil);
  // After the engine's own clear, so the twin ends in the same state whatever
  // the engine asked for, and a failure here leaves the vanilla clear done.
  shadowResMirrorClear(self, dsv, flags, depth, stencil);
}

}  // namespace atfix
