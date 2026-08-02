// SPDX-License-Identifier: MIT
//
// See msaa.h for why this is a twin-resource implementation and not a
// sample-count bump, and for the static analysis that closed the question of
// whether Ayesha's own renderer could be asked to do this instead.
//
// PROVENANCE. The twin/resolve mechanism is TellowKrinkle's, from the rendering
// work in his atelier-sync-fix fork, by way of the Arland project's adaptation
// in src/sync_fix.cpp (this project's own code, MIT). Two differences from
// Arland are worth stating up front, because they are where the risk lives:
//
//   1. Arland is a full D3D11 proxy and can see every context call. This
//      project hooks vtable slots, so every interception point had to be
//      chosen deliberately rather than inherited. Anything that reads a host
//      through a path not listed in msaa.h reads a stale surface.
//
//   2. Ayesha draws on a DEFERRED context. Arland's games mostly do not, and
//      its own notes record the deferred path as the part that needed the most
//      care. Here it is the normal case, not the exception.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "config.h"
#include "game.h"
#include "log.h"
#include "d3d11_hooks.h"
#include "msaa.h"
#include "smaa.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// Private-data keys. Distinct from the Arland project's own set on purpose:
// the two mods are never loaded into the same process, but colliding GUIDs
// across sibling codebases is the kind of thing that costs a day to find if it
// ever does happen.
const GUID IID_DuskMsaaTwin =
  { 0x7c1f4a20, 0x5d63, 0x4b8e, { 0x9a, 0x14, 0x2e, 0x77, 0x0b, 0x35, 0xc1, 0x01 } };
const GUID IID_DuskMsaaDirty =
  { 0x7c1f4a20, 0x5d63, 0x4b8e, { 0x9a, 0x14, 0x2e, 0x77, 0x0b, 0x35, 0xc1, 0x02 } };
const GUID IID_DuskMsaaBoundColor =
  { 0x7c1f4a20, 0x5d63, 0x4b8e, { 0x9a, 0x14, 0x2e, 0x77, 0x0b, 0x35, 0xc1, 0x03 } };
// The pair a bind displaced, held until the bind has actually happened. See
// msaa.h on msaaResolveReplaced for why the resolve cannot run before that.
const GUID IID_DuskMsaaPending =
  { 0x7c1f4a20, 0x5d63, 0x4b8e, { 0x9a, 0x14, 0x2e, 0x77, 0x0b, 0x35, 0xc1, 0x04 } };
// Marks a depth host that has a twin, purely so SRV binds of it can be counted.
// See the comment on resolveColor: this is the one open question this feature
// ships with, and it is cheaper to measure it than to argue about it.
// The scene colour last bound on this context, for the pre-UI SMAA boundary.
// Tracked whether or not MSAA itself is on, because SMAA needs the same answer.
const GUID IID_DuskSceneColor =
  { 0x7c1f4a20, 0x5d63, 0x4b8e, { 0x9a, 0x14, 0x2e, 0x77, 0x0b, 0x35, 0xc1, 0x06 } };
const GUID IID_DuskMsaaDepthHost =
  { 0x7c1f4a20, 0x5d63, 0x4b8e, { 0x9a, 0x14, 0x2e, 0x77, 0x0b, 0x35, 0xc1, 0x05 } };

enum class Dirty : UINT { Clean = 0, Written = 1 };

ID3D11Device* g_device = nullptr;
void (*g_bindTargets)(ID3D11DeviceContext*, unsigned int,
                      ID3D11RenderTargetView* const*,
                      ID3D11DepthStencilView*) = nullptr;
MsaaSceneTest g_sceneTest = nullptr;

// The descriptor shape of the first pair accepted, so a later pair that differs
// can be called out. See the use site for why shape rather than count.
struct PairShape {
  DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
  UINT colorBind = 0;
  DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
  UINT depthBind = 0;
  bool operator == (const PairShape& o) const {
    return colorFormat == o.colorFormat && colorBind == o.colorBind &&
           depthFormat == o.depthFormat && depthBind == o.depthBind;
  }
};
PairShape g_firstShape;

// Counters. This feature's predecessor reported itself active while doing
// nothing visible, so every number here answers a question that was previously
// answered by inference: did a twin get built, did a bind actually get
// substituted, did the contents ever get landed back.
std::atomic<uint64_t> g_twinPairs{0};
std::atomic<uint64_t> g_substitutions{0};
std::atomic<uint64_t> g_colorResolves{0};
std::atomic<uint64_t> g_declinedBinds{0};
std::atomic<uint64_t> g_twinFailures{0};
std::atomic<uint64_t> g_depthHostReads{0};
std::atomic<bool> g_reportedActive{false};

template <typename T>
T* twinOf(ID3D11DeviceChild* host) {
  T* object = nullptr;
  UINT size = sizeof(object);
  return host && SUCCEEDED(host->GetPrivateData(IID_DuskMsaaTwin, &size, &object))
    ? object : nullptr;
}

void markDirty(ID3D11Resource* host) {
  const Dirty state = Dirty::Written;
  if (host)
    host->SetPrivateData(IID_DuskMsaaDirty, sizeof(state), &state);
}

bool isDirty(ID3D11Resource* host) {
  Dirty state = Dirty::Clean;
  UINT size = sizeof(state);
  return host &&
         SUCCEEDED(host->GetPrivateData(IID_DuskMsaaDirty, &size, &state)) &&
         state == Dirty::Written;
}

void markClean(ID3D11Resource* host) {
  const Dirty state = Dirty::Clean;
  if (host)
    host->SetPrivateData(IID_DuskMsaaDirty, sizeof(state), &state);
}

// Land a colour twin back into its host.
//
// Depth is deliberately NOT handled here, and cannot be: D3D11's
// ResolveSubresource rejects depth-stencil formats outright. Only colour hosts
// are ever marked dirty, so only colour hosts ever reach this -- which is
// exactly what both prior-art implementations do, though in their case by
// accident rather than by statement. Neither TellowKrinkle's fork nor the
// Arland port ever marks a depth resource dirty, so neither has ever attempted
// the invalid resolve -- and neither could have detected it if they had, since
// ResolveSubresource returns void and reports a rejected call only through the
// debug layer.
//
// Whether that gap costs anything here is an open question and NOT one to
// settle by reasoning: it depends on whether this engine ever samples its scene
// depth host, and the evidence that it does was withdrawn (the sampler names it
// rested on belong to another game's shaders -- WORK_DOC.md). The depth host is
// therefore tagged at twin time and SRV binds of it are counted, so one run
// answers it. A non-zero `depthHostReads` in the log means the depth-resolve
// pass has to be written; zero means there is nothing to write.
void resolveColor(ID3D11DeviceContext* context, ID3D11Resource* host) {
  if (!isDirty(host))
    return;
  ID3D11Resource* twin = twinOf<ID3D11Resource>(host);
  if (!twin)
    return;
  // Drop the bindings around the resolve and put them back. A twin that is
  // still a bound render target cannot legally be a resolve source; see msaa.h
  // on msaaSetTargetBinder for why this saves and restores rather than merely
  // unbinding. Deferred contexts track this state the same way the immediate
  // one does, so the read back is valid on both.
  ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* savedDsv = nullptr;
  const bool rebind = g_bindTargets != nullptr;
  if (rebind) {
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                savedRtvs, &savedDsv);
    g_bindTargets(context, 0, nullptr, nullptr);
  }
  ID3D11Texture2D* texture = nullptr;
  if (SUCCEEDED(twin->QueryInterface(IID_ID3D11Texture2D,
                                     reinterpret_cast<void**>(&texture))) &&
      texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    texture->Release();
    // The twin was created with the render-target view's concrete format
    // precisely so this call has a legal format to name: the hosts are
    // typeless, and ResolveSubresource will not take a typeless format.
    context->ResolveSubresource(host, 0, twin, 0, desc.Format);
    markClean(host);
    g_colorResolves.fetch_add(1, std::memory_order_relaxed);
  }
  twin->Release();

  if (rebind) {
    g_bindTargets(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs,
                  savedDsv);
    for (auto* view : savedRtvs) {
      if (view)
        view->Release();
    }
    if (savedDsv)
      savedDsv->Release();
  }
}

// Whether a twin can exist for this texture at all. Structural only: these are
// the shapes the substitution and the resolve are defined for, and they hold on
// any engine. Whether this surface is the one worth multisampling is a separate
// question and not one core is entitled to answer -- see msaa.h on
// MsaaSceneTest.
bool twinnable(const D3D11_TEXTURE2D_DESC& desc) {
  return desc.SampleDesc.Count == 1 &&
         desc.ArraySize == 1 && desc.MipLevels == 1;
}

// The highest supported count at or below `want`, for this format on this
// device. Zero if even two samples are refused.
//
// This is what keeps an over-ambitious setting from turning into no MSAA at
// all: 8x is not universal, and a request the driver declines should quietly
// become 4x rather than silently becoming nothing. The Arland implementation
// walks down the same way, and for the same reason -- MSAA "never fails
// outright" is a documented property of that feature, not an accident.
UINT supportedSamples(DXGI_FORMAT format, UINT want) {
  for (UINT samples = want; samples >= 2; samples /= 2) {
    UINT quality = 0;
    if (SUCCEEDED(g_device->CheckMultisampleQualityLevels(format, samples,
                                                          &quality)) &&
        quality > 0)
      return samples;
  }
  return 0;
}

// Build the multisample twin for one host, or hand back the one already
// attached. `viewFormat` is the concrete format the game's own view names,
// which is what the twin is created with -- a typeless twin would resolve the
// same way the host does, which is to say not at all.
ID3D11Texture2D* getOrCreateTwin(ID3D11Resource* hostResource,
                                 DXGI_FORMAT viewFormat, UINT bindFlag,
                                 UINT samples) {
  if (ID3D11Texture2D* existing = twinOf<ID3D11Texture2D>(hostResource))
    return existing;

  ID3D11Texture2D* host = nullptr;
  if (FAILED(hostResource->QueryInterface(IID_ID3D11Texture2D,
                                          reinterpret_cast<void**>(&host))) ||
      !host)
    return nullptr;
  D3D11_TEXTURE2D_DESC desc = {};
  host->GetDesc(&desc);
  host->Release();

  desc.Format = viewFormat;
  desc.SampleDesc.Count = samples;
  desc.SampleDesc.Quality = 0;
  // Render-target or depth-stencil only. The host keeps the shader-resource
  // binding, because the host is what the game samples; a twin that could also
  // be sampled would just be a second way to read the wrong surface.
  desc.BindFlags = bindFlag;
  desc.CPUAccessFlags = 0;
  desc.MiscFlags = 0;
  desc.Usage = D3D11_USAGE_DEFAULT;

  ID3D11Texture2D* twin = nullptr;
  if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &twin)) || !twin) {
    g_twinFailures.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  // SetPrivateDataInterface ties the twin's lifetime to the host's: when the
  // engine releases its target, the twin goes with it. Nothing in this module
  // keeps a separate registry, which is what makes a resize or a device reset
  // survivable without a lifetime audit.
  hostResource->SetPrivateDataInterface(IID_DuskMsaaTwin, twin);
  return twin;
}

}  // namespace

unsigned int msaaSamples() {
  static const unsigned int samples = [] () -> unsigned int {
    if (featureSupport(Feature::Msaa) == Support::Unsupported)
      return 0;
    int v = 0;
    if (const char* env = std::getenv("DUSK_MSAA"))
      v = std::atoi(env);
    else
      v = duskConfigInt("Rendering", "MSAA", 0);
    if (v == 2 || v == 4 || v == 8)
      return unsigned(v);
    return 0;
  }();
  return samples;
}

bool msaaActive() {
  return msaaSamples() > 1 &&
         g_twinPairs.load(std::memory_order_relaxed) > 0;
}

void msaaInitialize(ID3D11Device* device) {
  g_device = device;
}

void msaaSetSceneTest(MsaaSceneTest test) {
  g_sceneTest = test;
}

bool msaaSubstituteTargets(ID3D11DeviceContext* context,
                           unsigned int numViews,
                           ID3D11RenderTargetView* const* views,
                           ID3D11DepthStencilView* depth,
                           ID3D11RenderTargetView** rtvOut,
                           ID3D11DepthStencilView** dsvOut) {
  *rtvOut = nullptr;
  *dsvOut = nullptr;
  const UINT samples = msaaSamples();
  if (!samples || !g_device || !context)
    return false;

  // Set aside whatever was bound until now. It is about to become readable by
  // something else, but it cannot be resolved yet -- it is still bound. Move
  // it to the pending slot and let msaaResolveReplaced land it once the real
  // bind has happened. This runs on every bind, including the ones we decline,
  // because the pair being replaced is what matters here, not the one arriving.
  ID3D11Resource* previous = nullptr;
  UINT previousSize = sizeof(previous);
  if (SUCCEEDED(context->GetPrivateData(IID_DuskMsaaBoundColor, &previousSize,
                                        &previous)) && previous) {
    context->SetPrivateData(IID_DuskMsaaBoundColor, 0, nullptr);
    context->SetPrivateDataInterface(IID_DuskMsaaPending, previous);
    previous->Release();
  }

  // Exactly one colour target and a depth target. The scene pass is the only
  // thing in these engines that binds that shape at the main render size, and
  // requiring the depth target is what makes the substitution safe: a
  // multisample colour target bound alongside a single-sample depth target is
  // an invalid combination D3D11 would reject.
  if (numViews != 1 || !views || !views[0] || !depth)
    return false;

  ID3D11RenderTargetView* hostRtv = views[0];
  ID3D11DepthStencilView* hostDsv = depth;

  ID3D11Resource* colorResource = nullptr;
  ID3D11Resource* depthResource = nullptr;
  hostRtv->GetResource(&colorResource);
  hostDsv->GetResource(&depthResource);
  bool substituted = false;

  ID3D11Texture2D* colorTexture = nullptr;
  ID3D11Texture2D* depthTexture = nullptr;
  if (colorResource && depthResource &&
      SUCCEEDED(colorResource->QueryInterface(
        IID_ID3D11Texture2D, reinterpret_cast<void**>(&colorTexture))) &&
      SUCCEEDED(depthResource->QueryInterface(
        IID_ID3D11Texture2D, reinterpret_cast<void**>(&depthTexture))) &&
      colorTexture && depthTexture) {
    D3D11_TEXTURE2D_DESC colorDesc = {};
    D3D11_TEXTURE2D_DESC depthDesc = {};
    colorTexture->GetDesc(&colorDesc);
    depthTexture->GetDesc(&depthDesc);

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    hostRtv->GetDesc(&rtvDesc);
    hostDsv->GetDesc(&dsvDesc);

    // One count has to satisfy both surfaces: a colour target and a depth
    // target bound together must agree on sample count, so the effective
    // count is the lower of what each format supports.
    const UINT colorCapable = supportedSamples(rtvDesc.Format, samples);
    const UINT depthCapable = supportedSamples(dsvDesc.Format, samples);
    const UINT effective =
      (colorCapable < depthCapable) ? colorCapable : depthCapable;

    // Structure first, then identity. The order matters only for the log: a
    // decline because nothing registered a scene test is a configuration
    // problem worth naming, while a decline because this is a shadow map is
    // the normal case and must stay silent.
    const bool structureOk =
      effective >= 2 && twinnable(colorDesc) && twinnable(depthDesc);
    const MsaaSceneTest sceneTest = g_sceneTest;
    if (structureOk && !sceneTest) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true, std::memory_order_relaxed))
        log("MSAA: no scene-target test is registered for this engine, so"
            " every bind is declined. MSAA does nothing this session.");
    }
    if (structureOk && sceneTest && sceneTest(colorDesc, depthDesc)) {
      ID3D11Texture2D* colorTwin = getOrCreateTwin(
        colorResource, rtvDesc.Format, D3D11_BIND_RENDER_TARGET, effective);
      ID3D11Texture2D* depthTwin = colorTwin ? getOrCreateTwin(
        depthResource, dsvDesc.Format, D3D11_BIND_DEPTH_STENCIL, effective)
        : nullptr;

      if (colorTwin && depthTwin) {
        // The views over the twins are cached on the game's own views, so the
        // second and every later bind of the same pair costs two private-data
        // reads rather than two view creations.
        ID3D11RenderTargetView* twinRtv = twinOf<ID3D11RenderTargetView>(hostRtv);
        ID3D11DepthStencilView* twinDsv = twinOf<ID3D11DepthStencilView>(hostDsv);
        if (!twinRtv || !twinDsv) {
          if (twinRtv) { twinRtv->Release(); twinRtv = nullptr; }
          if (twinDsv) { twinDsv->Release(); twinDsv = nullptr; }
          D3D11_RENDER_TARGET_VIEW_DESC twinRtvDesc = rtvDesc;
          D3D11_DEPTH_STENCIL_VIEW_DESC twinDsvDesc = dsvDesc;
          twinRtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
          twinDsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
          twinDsvDesc.Flags = 0;
          if (SUCCEEDED(g_device->CreateRenderTargetView(
                colorTwin, &twinRtvDesc, &twinRtv)) &&
              SUCCEEDED(g_device->CreateDepthStencilView(
                depthTwin, &twinDsvDesc, &twinDsv))) {
            hostRtv->SetPrivateDataInterface(IID_DuskMsaaTwin, twinRtv);
            hostDsv->SetPrivateDataInterface(IID_DuskMsaaTwin, twinDsv);

            // Clear the twin the moment it exists.
            //
            // A twin is created lazily, on the first BIND of a pair -- but the
            // engine's clear-then-bind-then-draw order means that frame's clear
            // has already happened, against the host, while no twin existed to
            // redirect it to. CreateTexture2D with null initial data leaves
            // undefined contents, so without this the first frame after MSAA
            // engages draws its opaque geometry on top of whatever was in that
            // memory, and depth-tests against garbage. One visible bad frame,
            // self-healing afterwards because every later clear finds the twin.
            const ContextOriginals& originals = d3d11OriginalsFor(context);
            if (originals.clearRenderTargetView && originals.clearDepthStencilView) {
              const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
              originals.clearRenderTargetView(context, twinRtv, black);
              originals.clearDepthStencilView(context, twinDsv,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            }

            const uint64_t pairs =
              g_twinPairs.fetch_add(1, std::memory_order_relaxed);
            // Every twinned pair is described exactly once. Identifying an
            // over-match needs the format and bind flags of both surfaces --
            // size alone is what let a wrong pair in once, so size alone cannot
            // tell them apart afterwards.
            log("MSAA: twinned pair #", std::dec, pairs + 1, " ",
                colorDesc.Width, "x", colorDesc.Height,
                " colour format=", uint32_t(colorDesc.Format),
                " bindFlags=0x", std::hex, colorDesc.BindFlags, std::dec,
                " rtvFormat=", uint32_t(rtvDesc.Format),
                " | depth format=", uint32_t(depthDesc.Format),
                " bindFlags=0x", std::hex, depthDesc.BindFlags, std::dec,
                " dsvFormat=", uint32_t(dsvDesc.Format));

            // The anomaly signal is a pair of a DIFFERENT SHAPE, not a second
            // pair. An engine that ping-pongs between two identically-shaped
            // scene targets for its post-processing chain is doing something
            // completely ordinary, and Ayesha does exactly that -- warning on
            // count alone cried wolf on the normal case while saying nothing
            // about what actually differed. Shape is the thing that separates
            // a legitimate second scene target from a surface the test should
            // never have accepted.
            const PairShape shape{ colorDesc.Format, colorDesc.BindFlags,
                                   depthDesc.Format, depthDesc.BindFlags };
            if (pairs == 0) {
              g_firstShape = shape;
              log("FIXES msaa=active samples=", std::dec, effective, " size=",
                  colorDesc.Width, "x", colorDesc.Height);
            } else if (!(shape == g_firstShape)) {
              log("MSAA: WARNING pair #", std::dec, pairs + 1, " has a"
                  " different shape from the first one twinned -- the scene"
                  " test is accepting more than one kind of surface, and one"
                  " of them is probably not the scene");
            }
          } else {
            if (twinRtv) { twinRtv->Release(); twinRtv = nullptr; }
            if (twinDsv) { twinDsv->Release(); twinDsv = nullptr; }
            g_twinFailures.fetch_add(1, std::memory_order_relaxed);
          }
        }

        if (twinRtv && twinDsv) {
          // The twin now holds the scene, so the host is stale until something
          // reads it and the resolve above lands it.
          markDirty(colorResource);
          const UINT depthMarker = 1;
          depthResource->SetPrivateData(IID_DuskMsaaDepthHost,
                                        sizeof(depthMarker), &depthMarker);
          context->SetPrivateDataInterface(IID_DuskMsaaBoundColor, colorResource);
          // A re-bind of the pair that is already substituted is not a
          // displacement, but the unconditional move at the top of this
          // function has already put it in the pending slot. Letting
          // msaaResolveReplaced land it from there would run AFTER the
          // markDirty above and mark the host clean -- while the twin, just
          // re-bound, goes on accumulating newer content. Every later read of
          // the host would then skip its resolve, which is precisely the
          // stale-surface read this module exists to prevent. The Arland
          // implementation is immune by ordering (resolveBoundMSAA runs before
          // its Dirty store); here the pending entry is dropped instead:
          // nothing moved off the twin, so there is nothing to land.
          ID3D11Resource* pending = nullptr;
          UINT pendingSize = sizeof(pending);
          if (SUCCEEDED(context->GetPrivateData(IID_DuskMsaaPending,
                                                &pendingSize, &pending)) &&
              pending) {
            if (pending == colorResource)
              context->SetPrivateData(IID_DuskMsaaPending, 0, nullptr);
            pending->Release();
          }
          *rtvOut = twinRtv;
          *dsvOut = twinDsv;
          substituted = true;
          g_substitutions.fetch_add(1, std::memory_order_relaxed);
        } else {
          if (twinRtv) twinRtv->Release();
          if (twinDsv) twinDsv->Release();
        }
        colorTwin->Release();
        depthTwin->Release();
      } else {
        if (colorTwin) colorTwin->Release();
        if (depthTwin) depthTwin->Release();
      }
    } else {
      g_declinedBinds.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (colorTexture) colorTexture->Release();
  if (depthTexture) depthTexture->Release();
  if (colorResource) colorResource->Release();
  if (depthResource) depthResource->Release();
  return substituted;
}

void msaaResolveShaderResources(ID3D11DeviceContext* context,
                                unsigned int numViews,
                                ID3D11ShaderResourceView* const* views) {
  if (!msaaSamples() || !views || !context)
    return;
  for (unsigned int i = 0; i < numViews; ++i) {
    if (!views[i])
      continue;
    ID3D11Resource* resource = nullptr;
    views[i]->GetResource(&resource);
    if (resource) {
      // Count reads of a twinned depth host before resolving anything. This is
      // the measurement that decides whether a depth-resolve pass is needed at
      // all; the surface it names is stale by construction, so a non-zero count
      // is a real defect rather than a curiosity.
      UINT marker = 0;
      UINT markerSize = sizeof(marker);
      if (SUCCEEDED(resource->GetPrivateData(IID_DuskMsaaDepthHost,
                                             &markerSize, &marker)) && marker)
        g_depthHostReads.fetch_add(1, std::memory_order_relaxed);
      resolveColor(context, resource);
      resource->Release();
    }
  }
}

void msaaResolveCopySource(ID3D11DeviceContext* context,
                           ID3D11Resource* source) {
  if (!msaaSamples() || !source || !context)
    return;
  resolveColor(context, source);
}

void msaaResolveReplaced(ID3D11DeviceContext* context) {
  if (!msaaSamples() || !context)
    return;
  ID3D11Resource* host = nullptr;
  UINT size = sizeof(host);
  if (FAILED(context->GetPrivateData(IID_DuskMsaaPending, &size, &host)) ||
      !host)
    return;
  context->SetPrivateData(IID_DuskMsaaPending, 0, nullptr);
  resolveColor(context, host);
  host->Release();
}

void msaaResolveBeforeFinish(ID3D11DeviceContext* context) {
  if (!msaaSamples() || !context)
    return;
  // Both slots, in order: a command list can be closed with the scene pair
  // still bound, and it can also be closed with a displaced pair that no
  // intervening bind ever landed.
  msaaResolveReplaced(context);
  ID3D11Resource* host = nullptr;
  UINT size = sizeof(host);
  if (FAILED(context->GetPrivateData(IID_DuskMsaaBoundColor, &size, &host)) ||
      !host)
    return;
  context->SetPrivateData(IID_DuskMsaaBoundColor, 0, nullptr);
  resolveColor(context, host);
  host->Release();
}

void msaaNoteSceneBoundary(ID3D11DeviceContext* context, unsigned int numViews,
                           ID3D11RenderTargetView* const* views,
                           ID3D11DepthStencilView* depth) {
  const MsaaSceneTest sceneTest = g_sceneTest;
  if (!context || !sceneTest || !smaaPreUiEnabled())
    return;

  // Is the arriving bind the scene?
  ID3D11Texture2D* arriving = nullptr;
  if (numViews == 1 && views && views[0] && depth) {
    ID3D11Resource* colorResource = nullptr;
    ID3D11Resource* depthResource = nullptr;
    views[0]->GetResource(&colorResource);
    depth->GetResource(&depthResource);
    ID3D11Texture2D* colorTex = nullptr;
    ID3D11Texture2D* depthTex = nullptr;
    if (colorResource && depthResource &&
        SUCCEEDED(colorResource->QueryInterface(IID_ID3D11Texture2D,
          reinterpret_cast<void**>(&colorTex))) &&
        SUCCEEDED(depthResource->QueryInterface(IID_ID3D11Texture2D,
          reinterpret_cast<void**>(&depthTex))) && colorTex && depthTex) {
      D3D11_TEXTURE2D_DESC cd = {};
      D3D11_TEXTURE2D_DESC dd = {};
      colorTex->GetDesc(&cd);
      depthTex->GetDesc(&dd);
      if (sceneTest(cd, dd)) {
        arriving = colorTex;      // ownership moves to `arriving`
        colorTex = nullptr;
      }
    }
    if (colorTex) colorTex->Release();
    if (depthTex) depthTex->Release();
    if (colorResource) colorResource->Release();
    if (depthResource) depthResource->Release();
  }

  ID3D11Texture2D* previous = nullptr;
  UINT size = sizeof(previous);
  if (FAILED(context->GetPrivateData(IID_DuskSceneColor, &size, &previous)))
    previous = nullptr;

  // Scene -> not-scene is the boundary. Under MSAA the host arriving here is
  // the one the twin has already been resolved into, because the resolve for
  // the displaced pair runs before this bind completes.
  if (previous && !arriving)
    smaaApplySceneColor(context, previous);

  if (arriving)
    context->SetPrivateDataInterface(IID_DuskSceneColor, arriving);
  else if (previous)
    context->SetPrivateData(IID_DuskSceneColor, 0, nullptr);

  if (previous) previous->Release();
  if (arriving) arriving->Release();
}

void msaaSetTargetBinder(
    void (*bind)(ID3D11DeviceContext*, unsigned int,
                 ID3D11RenderTargetView* const*, ID3D11DepthStencilView*)) {
  g_bindTargets = bind;
}

void msaaAdjustRasterizerState(D3D11_RASTERIZER_DESC* desc) {
  // Without this the twins are multisampled and the rasteriser still produces
  // single-sample coverage: every counter in this file would report success and
  // the picture would not change. That is exactly the failure this feature was
  // rewritten to stop making.
  if (desc && msaaSamples() > 1)
    desc->MultisampleEnable = TRUE;
}

void msaaResolveBeforePresent(IDXGISwapChain* swapChain) {
  if (!msaaSamples() || !swapChain || !g_device)
    return;
  ID3D11DeviceContext* context = nullptr;
  g_device->GetImmediateContext(&context);
  if (!context)
    return;
  msaaResolveBeforeFinish(context);
  context->Release();
}

void msaaFrameTick() {
  if (!msaaSamples())
    return;
  // One line, once, when the picture is genuinely multisampled -- and a
  // different line when it is configured and nothing attached, because those
  // two states were indistinguishable in the version this replaces.
  if (!g_reportedActive.load(std::memory_order_relaxed) &&
      g_substitutions.load(std::memory_order_relaxed) > 0) {
    g_reportedActive.store(true, std::memory_order_relaxed);
    log("MSAA: engaged twinPairs=", std::dec,
        g_twinPairs.load(std::memory_order_relaxed),
        " substitutions=", g_substitutions.load(std::memory_order_relaxed),
        " colorResolves=", g_colorResolves.load(std::memory_order_relaxed));
  }

  static std::atomic<uint64_t> frames{0};
  const uint64_t frame = frames.fetch_add(1, std::memory_order_relaxed) + 1;

  // The complement of the "engaged" line, once, after enough frames that
  // startup cannot explain it: configured, hooked, and not one bind ever
  // matched. The first version of this feature spent whole sessions in exactly
  // that state while every line it logged looked healthy, so the silence gets
  // named instead of left to be inferred from counters that stay at zero.
  // (The registered-no-test case has its own line in msaaSubstituteTargets;
  // this one means a test exists and declines everything -- see the note in
  // scene_target.cpp on what Ayesha's depends on.)
  if (frame == 1800 &&
      g_substitutions.load(std::memory_order_relaxed) == 0) {
    log("MSAA: configured but nothing engaged after ", std::dec, frame,
        " frames -- no bind matched the scene test (declinedBinds=",
        g_declinedBinds.load(std::memory_order_relaxed),
        " twinFailures=", g_twinFailures.load(std::memory_order_relaxed),
        "); the picture is not multisampled");
  }

  // A periodic line, because the interesting numbers are the ones that grow.
  // depthHostReads answers the open depth question; twinFailures and
  // declinedBinds are what a session that configured MSAA and got nothing would
  // have to show for itself.
  if (frame % 600 == 0) {
    log("MSAA twinPairs=", std::dec,
        g_twinPairs.load(std::memory_order_relaxed),
        " substitutions=", g_substitutions.load(std::memory_order_relaxed),
        " colorResolves=", g_colorResolves.load(std::memory_order_relaxed),
        " depthHostReads=", g_depthHostReads.load(std::memory_order_relaxed),
        " declinedBinds=", g_declinedBinds.load(std::memory_order_relaxed),
        " twinFailures=", g_twinFailures.load(std::memory_order_relaxed));
  }
}

// ---- the MSAA interception points -----------------------------------------
//
// Each of these forwards unconditionally and asks msaa.cpp what else to do.
// None of them changes behaviour when MSAA is off: msaa.cpp's entry points all
// return immediately on a zero sample count, so an ordinary session pays a
// predictable-branch call per intercepted method and nothing else.

// Bind through the trampoline rather than the public method, so a resolve can
// drop and restore the twin without re-entering this hook. msaa.cpp holds this
// as a callback; see msaa.h on msaaSetTargetBinder.
void msaaBindTargets(ID3D11DeviceContext* context, unsigned int numViews,
                       ID3D11RenderTargetView* const* views,
                       ID3D11DepthStencilView* depth) {
  const ContextOriginals& originals = d3d11OriginalsFor(context);
  if (originals.omSetRenderTargets)
    originals.omSetRenderTargets(context, numViews, views, depth);
}

void STDMETHODCALLTYPE hookedOMSetRenderTargets(
    ID3D11DeviceContext* self, UINT numViews,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) {
  // The scene/UI boundary, observed. This bind is the composite if the pair
  // that was bound until now was the scene and this one is not -- at which
  // point the scene is complete, the interface has not been drawn, and the
  // scene target is exactly the surface SMAA wants. See smaa.h.
  msaaNoteSceneBoundary(self, numViews, views, depth);

  ID3D11RenderTargetView* twinRtv = nullptr;
  ID3D11DepthStencilView* twinDsv = nullptr;
  const bool substituted =
    msaaSubstituteTargets(self, numViews, views, depth, &twinRtv, &twinDsv);

  // Land the pair this bind displaces before issuing the bind. Either side of
  // the forward would be legal -- the resolve saves, drops and restores the
  // render-target bindings around itself -- but doing it here means the game's
  // own bind, not this module's restore, has the last word on the context's
  // state.
  msaaResolveReplaced(self);

  if (substituted) {
    ID3D11RenderTargetView* substituteViews[1] = { twinRtv };
    d3d11OriginalsFor(self).omSetRenderTargets(self, 1, substituteViews, twinDsv);
    twinRtv->Release();
    twinDsv->Release();
  } else {
    d3d11OriginalsFor(self).omSetRenderTargets(self, numViews, views, depth);
  }
}

void STDMETHODCALLTYPE hookedPSSetShaderResources(
    ID3D11DeviceContext* self, UINT startSlot, UINT numViews,
    ID3D11ShaderResourceView* const* views) {
  // Before the bind, not after: the point is that the game is about to sample
  // these, and the resolve has to have happened by then.
  msaaResolveShaderResources(self, numViews, views);
  d3d11OriginalsFor(self).psSetShaderResources(self, startSlot, numViews, views);
}

void STDMETHODCALLTYPE hookedCopyResource(
    ID3D11DeviceContext* self, ID3D11Resource* dst, ID3D11Resource* src) {
  msaaResolveCopySource(self, src);
  d3d11OriginalsFor(self).copyResource(self, dst, src);
}

void STDMETHODCALLTYPE hookedCopySubresourceRegion(
    ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSubresource,
    UINT dstX, UINT dstY, UINT dstZ, ID3D11Resource* src, UINT srcSubresource,
    const D3D11_BOX* box) {
  msaaResolveCopySource(self, src);
  d3d11OriginalsFor(self).copySubresourceRegion(self, dst, dstSubresource, dstX,
                                           dstY, dstZ, src, srcSubresource,
                                           box);
}

HRESULT STDMETHODCALLTYPE hookedFinishCommandList(
    ID3D11DeviceContext* self, BOOL restoreState,
    ID3D11CommandList** commandList) {
  // This is the one that matters most on Ayesha. The engine records its scene
  // into a deferred context and closes the list; whatever is still bound at
  // that moment has to be landed before the list is replayed, or the resolve
  // would be recorded after the reads it is supposed to precede.
  msaaResolveBeforeFinish(self);
  return d3d11OriginalsFor(self).finishCommandList(self, restoreState, commandList);
}

// ---- the rasterizer-state hook --------------------------------------------
//
// Multisampled targets alone do not produce multisampled output: a rasterizer
// state with MultisampleEnable clear emits single-sample coverage into them and
// the picture is unchanged. Every counter this feature keeps would still report
// success, which is the failure mode this whole rewrite exists to stop
// repeating, so the state is corrected at creation for as long as MSAA is on.
// Replaying a recorded list resets or overwrites this context's render-target
// state, so whatever it still holds as bound has to be landed first.
//
// Ayesha records its scene on a deferred context and replays it here, and the
// markers this module keeps are per-context private data -- so the deferred
// context's marker is already cleared by the FinishCommandList detour. This
// covers the immediate context's own, which nothing else would.
void STDMETHODCALLTYPE hookedExecuteCommandList(
    ID3D11DeviceContext* self, ID3D11CommandList* commandList,
    BOOL restoreContextState) {
  msaaResolveBeforeFinish(self);
  d3d11OriginalsFor(self).executeCommandList(self, commandList,
                                             restoreContextState);
}

// ---- the clears -----------------------------------------------------------
//
// The game clears the view IT created. Under substitution that view is the
// host, while the twin is what is bound and being drawn into -- so an
// unintercepted clear zeroes a surface nobody is rendering to and leaves the
// twin holding the previous frame. The visible result is every frame composited
// on top of the last, which is not subtle and is exactly what the first version
// of this feature would have shipped.
//
// The twin is cleared INSTEAD of the host, not as well as it: the host's
// contents are overwritten wholesale by the resolve, so clearing it is work
// with no observer. Both reference implementations do the same
// (sync_fix.cpp:2934, impl.cpp:1579).
void STDMETHODCALLTYPE hookedClearRenderTargetView(
    ID3D11DeviceContext* self, ID3D11RenderTargetView* view,
    const FLOAT color[4]) {
  ID3D11RenderTargetView* twin =
    msaaSamples() > 1 ? twinOf<ID3D11RenderTargetView>(view) : nullptr;
  d3d11OriginalsFor(self).clearRenderTargetView(self, twin ? twin : view,
                                                color);
  if (twin)
    twin->Release();
}

void STDMETHODCALLTYPE hookedClearDepthStencilView(
    ID3D11DeviceContext* self, ID3D11DepthStencilView* view, UINT flags,
    FLOAT depth, UINT8 stencil) {
  ID3D11DepthStencilView* twin =
    msaaSamples() > 1 ? twinOf<ID3D11DepthStencilView>(view) : nullptr;
  d3d11OriginalsFor(self).clearDepthStencilView(self, twin ? twin : view, flags,
                                                depth, stencil);
  if (twin)
    twin->Release();
}

// The other route to binding render targets. Nothing in these engines is known
// to use it, but "known" here means "never looked", and a bind that arrives
// this way would silently bypass the substitution and leave the scene
// single-sampled while every counter reported success -- the same shape of
// failure this feature was rewritten to stop making. Both reference
// implementations hook it (sync_fix.cpp:3513, impl.cpp:1745).
void STDMETHODCALLTYPE hookedOMSetRenderTargetsAndUnorderedAccessViews(
    ID3D11DeviceContext* self, UINT numViews,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth,
    UINT uavStart, UINT numUavs, ID3D11UnorderedAccessView* const* uavs,
    const UINT* uavInitialCounts) {
  // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL means "leave the render targets
  // exactly as they are and only touch the UAVs". There is no bind to
  // substitute, and treating the sentinel as a count would be a wild read.
  const bool keepTargets = numViews == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;

  ID3D11RenderTargetView* twinRtv = nullptr;
  ID3D11DepthStencilView* twinDsv = nullptr;
  const bool substituted = !keepTargets &&
    msaaSubstituteTargets(self, numViews, views, depth, &twinRtv, &twinDsv);

  if (!keepTargets)
    msaaResolveReplaced(self);

  if (substituted) {
    ID3D11RenderTargetView* substituteViews[1] = { twinRtv };
    d3d11OriginalsFor(self).omSetRenderTargetsAndUnorderedAccessViews(
      self, 1, substituteViews, twinDsv, uavStart, numUavs, uavs,
      uavInitialCounts);
    twinRtv->Release();
    twinDsv->Release();
  } else {
    d3d11OriginalsFor(self).omSetRenderTargetsAndUnorderedAccessViews(
      self, numViews, views, depth, uavStart, numUavs, uavs, uavInitialCounts);
  }
}

HRESULT STDMETHODCALLTYPE hookedCreateRasterizerState(
    ID3D11Device* self, const D3D11_RASTERIZER_DESC* desc,
    ID3D11RasterizerState** state) {
  if (!desc)
    return d3d11DeviceOriginals().createRasterizerState(self, desc, state);
  D3D11_RASTERIZER_DESC local = *desc;
  msaaAdjustRasterizerState(&local);
  return d3d11DeviceOriginals().createRasterizerState(self, &local, state);
}

}  // namespace atfix
