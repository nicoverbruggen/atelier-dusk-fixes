// SPDX-License-Identifier: MIT
//
// See d3d11_hooks.h for why one module owns these vtables.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstddef>
#include <cstdint>

#include "d3d11_hooks.h"
#include "highres.h"
#include "log.h"
#include "sampler.h"
#include "scene_pass.h"
#include "smaa.h"
#include "supersample.h"
#include "../../vendor/minhook/include/MinHook.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

DeviceOriginals g_deviceOriginals;
ContextOriginals g_immediateOriginals;
ContextOriginals g_deferredOriginals;
void** g_immediateVtable = nullptr;
bool g_installed = false;

// One row per hooked context method. `originalOffset` names where in
// ContextOriginals the trampoline belongs, so the table stays declarative and
// the install loop stays a loop.
struct ContextHookSpec {
  int slot;
  void* detour;
  size_t originalOffset;
  const char* name;
};

#define ORIGINAL_AT(member) offsetof(ContextOriginals, member)

// The raster correction's set: the two raster submissions it has to notice, and
// the four draws at which it applies the correction. Owned by highres.cpp.
const ContextHookSpec kRasterHooks[] = {
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

// The scene-pass set: the two bind points that carry the scene/UI boundary, the
// sample the composite makes, and the close of a recorded list. Owned by
// scene_pass.cpp, and wanted by whichever of SMAA and supersampling is on.
const ContextHookSpec kScenePassHooks[] = {
  { 33, reinterpret_cast<void*>(&hookedOMSetRenderTargets),
    ORIGINAL_AT(omSetRenderTargets), "OMSetRenderTargets" },
  { 8, reinterpret_cast<void*>(&hookedPSSetShaderResources),
    ORIGINAL_AT(psSetShaderResources), "PSSetShaderResources" },
  { 114, reinterpret_cast<void*>(&hookedFinishCommandList),
    ORIGINAL_AT(finishCommandList), "FinishCommandList" },
  // The other way to bind render targets. Nothing in these engines is known to
  // use it, but "known" here means "never looked".
  { 34,
    reinterpret_cast<void*>(&hookedOMSetRenderTargetsAndUnorderedAccessViews),
    ORIGINAL_AT(omSetRenderTargetsAndUnorderedAccessViews),
    "OMSetRenderTargetsAndUnorderedAccessViews" },
};

#undef ORIGINAL_AT

// Hook one context's vtable from one spec table, filling `originals` with the
// trampolines. Enabling happens per entry rather than in a second pass because
// the two vtables are independent: a deferred context that cannot be hooked is
// a reason to decline the whole install, and the caller undoes the device set.
bool hookContextVtable(ID3D11DeviceContext* target, ContextOriginals& originals,
                       const ContextHookSpec* specs, int count,
                       const char* which) {
  auto** vtable = *reinterpret_cast<void***>(target);
  auto* base = reinterpret_cast<uint8_t*>(&originals);
  for (int i = 0; i < count; ++i) {
    void* fn = vtable[specs[i].slot];
    void** slot = reinterpret_cast<void**>(base + specs[i].originalOffset);
    const MH_STATUS created = MH_CreateHook(fn, specs[i].detour, slot);
    // The two vtables can legitimately share an entry -- an implementation is
    // free to give both context types the same function for a method that does
    // not differ. MinHook refuses the second hook on that address, which is not
    // an error: the first install already covers it. Reuse the trampoline it
    // produced, which is the one recorded in the immediate set, since that
    // vtable is always hooked first.
    if (created == MH_ERROR_ALREADY_CREATED) {
      auto* immediateBase = reinterpret_cast<uint8_t*>(&g_immediateOriginals);
      *slot = *reinterpret_cast<void**>(
        immediateBase + specs[i].originalOffset);
      continue;
    }
    if (created != MH_OK) {
      log("D3D11HOOKS: MH_CreateHook(", which, "::", specs[i].name,
          ") failed: ", MH_StatusToString(created));
      return false;
    }
    if (MH_EnableHook(fn) != MH_OK) {
      log("D3D11HOOKS: MH_EnableHook(", which, "::", specs[i].name, ") failed");
      return false;
    }
  }
  return true;
}

}  // namespace

const DeviceOriginals& d3d11DeviceOriginals() { return g_deviceOriginals; }

const ContextOriginals& d3d11OriginalsFor(ID3D11DeviceContext* context) {
  return *reinterpret_cast<void***>(context) == g_immediateVtable
    ? g_immediateOriginals : g_deferredOriginals;
}

// The ordering discipline every installer in this tree follows: a failure
// partway through must leave the process in the state it started in, not in a
// half-hooked one. That matters more here than anywhere else -- a live
// CreateTexture2D hook with a dead raster correction resizes the targets and
// leaves the viewport behind, which is a visibly broken frame rather than an
// unfixed one.
//
// An early version could reach exactly that state silently: the context hooks
// were skipped when no context was available, and it still logged a flat
// "installed fix=1". So every failure path below unwinds what it installed and
// declines the whole set, and the summary line names what actually went in.
void d3d11SetRenderTargets(ID3D11DeviceContext* context, UINT numViews,
                           ID3D11RenderTargetView* const* views,
                           ID3D11DepthStencilView* depth) {
  if (!context)
    return;
  const ContextOriginals& originals = d3d11OriginalsFor(context);
  if (originals.omSetRenderTargets)
    originals.omSetRenderTargets(context, numViews, views, depth);
  else
    context->OMSetRenderTargets(numViews, views, depth);
}

void d3d11InstallHooks(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (g_installed || !device)
    return;

  // Ask each feature what it needs before touching anything, so a session that
  // wants nothing installs nothing.
  const HighResWants highRes = highResResolveWants();
  // Supersampling owns no hook of its own, but it is not hookless either: it
  // reads the bind detour to identify the composite and the shader-resource
  // detour to substitute at its sample, and both live in the scene-pass set
  // below. Naming it here is what keeps `DUSK_SSAA=200` with everything else
  // off from installing nothing and reporting nothing.
  if (!highRes.createTexture2D && !highRes.rasterCorrection &&
      !anisotropyLevel() && !smaaPreUiEnabled() && !ssaaActive())
    return;

  // The Phyre module initializes MinHook for Ayesha, but this subsystem is the
  // only thing in the tree that hooks anything on Escha & Logy or Shallie, so
  // it cannot assume someone else has. MinHook answers a second call with
  // MH_ERROR_ALREADY_INITIALIZED, which is a success for our purposes.
  const MH_STATUS init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
    log("D3D11HOOKS: MH_Initialize failed (", MH_StatusToString(init),
        "), installing nothing");
    return;
  }

  auto** deviceVtable = *reinterpret_cast<void***>(device);

  // If no context came with the device, ask the device for its own rather than
  // installing a half-fix. This is the case an early version got wrong: the
  // raster correction was quietly skipped and only the resize went in.
  ID3D11DeviceContext* owned = nullptr;
  if (!context) {
    device->GetImmediateContext(&owned);
    context = owned;
  }

  // Context hooks are grouped, and each group goes in only when its feature is
  // on. The census alone needs no raster correction -- a diagnostic that
  // changed raster state would not be a diagnostic -- and a session with
  // neither SMAA nor supersampling should not pay for four detours on calls
  // this hot.
  ContextHookSpec specs[32];
  int specCount = 0;
  auto append = [&](const ContextHookSpec* from, int count) {
    for (int i = 0; i < count && specCount < int(sizeof(specs) / sizeof(specs[0]));
         ++i)
      specs[specCount++] = from[i];
  };
  // Also when supersampling is on: the enlarged scene targets are useless
  // without the raster correction that carries the engine's hard-coded viewport
  // and scissor onto them.
  //
  // NOT for the pre-UI SMAA path, though a version of this line briefly said so
  // on the grounds that SMAA "fires after the composite draw, and the draw
  // detours are in this set". It does not: it fires from scenePassNoteBoundary
  // inside the OMSetRenderTargets detour, which is in the scene-pass set below,
  // and there is no draw-time entry point anywhere in smaa.cpp. Installing six
  // detours on the hottest functions in the frame to satisfy a dependency that
  // does not exist is worth avoiding even when they are inert.
  if (highRes.rasterCorrection || ssaaConfigured())
    append(kRasterHooks, int(sizeof(kRasterHooks) / sizeof(kRasterHooks[0])));
  // The pre-UI SMAA path needs the OMSetRenderTargets detour to see the
  // scene/UI boundary, and supersampling identifies the composite from that
  // same detour and substitutes at PSSetShaderResources. Both live here.
  if (smaaPreUiEnabled() || ssaaActive())
    append(kScenePassHooks,
           int(sizeof(kScenePassHooks) / sizeof(kScenePassHooks[0])));

  if (specCount && !context) {
    log("D3D11HOOKS: no immediate context available, so the context hooks"
        " cannot be installed; declining to install anything, because a"
        " resize without its raster correction is a visibly broken frame");
    if (owned)
      owned->Release();
    return;
  }

  // Device slot 5. Wanted by the resolution fix and by the census alike, so it
  // goes in whenever either is on, and its failure is fatal to both.
  void* createTarget = deviceVtable[5];
  if (highRes.createTexture2D) {
    if (MH_CreateHook(createTarget,
          reinterpret_cast<void*>(&hookedCreateTexture2D),
          reinterpret_cast<void**>(&g_deviceOriginals.createTexture2D))
            != MH_OK ||
        MH_EnableHook(createTarget) != MH_OK) {
      log("D3D11HOOKS: could not hook CreateTexture2D, installing nothing");
      if (owned)
        owned->Release();
      return;
    }
  }

  // Device slot 23, CreateSamplerState. Cheap and unconditional when on: the
  // upgrade is a descriptor rewrite at creation time, and sampler states are
  // created a handful of times per session, not per frame.
  if (anisotropyLevel()) {
    void* samplerTarget = deviceVtable[23];
    if (MH_CreateHook(samplerTarget,
          reinterpret_cast<void*>(&hookedCreateSamplerState),
          reinterpret_cast<void**>(&g_deviceOriginals.createSamplerState))
            != MH_OK ||
        MH_EnableHook(samplerTarget) != MH_OK) {
      log("D3D11HOOKS: could not hook CreateSamplerState; anisotropic"
          " filtering will not engage this session");
    }
  }

  int hookedVtables = 0;
  if (specCount) {
    g_immediateVtable = *reinterpret_cast<void***>(context);
    if (!hookContextVtable(context, g_immediateOriginals, specs, specCount,
                           "immediate")) {
      if (highRes.createTexture2D)
        MH_DisableHook(createTarget);
      if (owned)
        owned->Release();
      return;
    }
    ++hookedVtables;

    // And the deferred context's vtable, which is where Ayesha actually draws.
    // One is created purely to read its vtable and then released; every
    // deferred context the game makes shares that vtable, so hooking through
    // ours covers all of them.
    ID3D11DeviceContext* deferred = nullptr;
    const HRESULT hr = device->CreateDeferredContext(0, &deferred);
    if (FAILED(hr) || !deferred) {
      log("D3D11HOOKS: CreateDeferredContext failed (hr=0x", std::hex,
          uint32_t(hr), std::dec, "); the context hooks would miss every draw"
          " this engine issues, so installing nothing");
      if (highRes.createTexture2D)
        MH_DisableHook(createTarget);
      if (owned)
        owned->Release();
      return;
    }
    void** deferredVtable = *reinterpret_cast<void***>(deferred);
    const bool distinct = deferredVtable != g_immediateVtable;
    log("D3D11HOOKS: context vtables immediate=",
        reinterpret_cast<void*>(g_immediateVtable),
        " deferred=", reinterpret_cast<void*>(deferredVtable),
        distinct ? " (distinct, both hooked)" : " (shared, one hook set)");
    bool ok = true;
    if (distinct) {
      ok = hookContextVtable(deferred, g_deferredOriginals, specs, specCount,
                             "deferred");
      if (ok)
        ++hookedVtables;
    } else {
      // One vtable serves both; the immediate set already covers it.
      g_deferredOriginals = g_immediateOriginals;
    }
    deferred->Release();
    if (!ok) {
      if (highRes.createTexture2D)
        MH_DisableHook(createTarget);
      if (owned)
        owned->Release();
      return;
    }
  }

  highResNoteImmediateContext(context);

  // Only the OFF half of supersampling's configured line is logged here. The ON
  // half carries the scene and display sizes, and neither exists yet: Ayesha
  // creates its device before its swap chain, and the main render size is
  // learned from the first depth target after that. ssaaFrameTick emits it once
  // those numbers are real, so the line never has to say "unknown".
  if (!ssaaConfigured())
    log("FIXES ssaa=off");

  // The trap that costs a whole session when it is hit. DUSK_HIGHRES=0 stands
  // down the CreateTexture2D hook, which is the only thing that ever learns a
  // main render size -- so Ayesha's scene test can never match, and there is
  // nothing enlarged for supersampling to downscale. Both features that read
  // the scene pass then report themselves configured and do nothing, which is
  // exactly the state this project has now been in three times.
  if (!highRes.rasterCorrection && (smaaPreUiEnabled() || ssaaConfigured()))
    log("D3D11HOOKS: WARNING the high-resolution fix is off, so no main render"
        " size is ever learned; the scene test cannot match and neither the"
        " pre-UI SMAA pass nor supersampling will do anything this session");

  log("D3D11HOOKS: installed highres=", highRes.rasterCorrection ? 1 : 0,
      " census=", highRes.createTexture2D && !highRes.rasterCorrection ? 1 : 0,
      " ssaa=", ssaaPercent(),
      " aniso=", anisotropyLevel(),
      " contextVtables=", hookedVtables,
      " hooksPerVtable=", specCount);

  // Latched only on success, matching main.cpp's hookPresent: a failed attempt
  // leaves a later device free to try again rather than doing nothing for the
  // rest of the session.
  g_installed = true;
  if (owned)
    owned->Release();
}

}  // namespace atfix
