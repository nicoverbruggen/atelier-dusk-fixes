// SPDX-License-Identifier: MIT
//
// See d3d11_hooks.h for why one module owns these vtables.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstddef>
#include <cstdint>

#include "d3d11_hooks.h"
#include "hook_util.h"
#include "highres.h"
#include "log.h"
#include "sampler.h"
#include "scene_pass.h"
#include "sharpen.h"
#include "smaa.h"
#include "frame_map.h"
#include "scene_policy.h"
#include "supersample.h"
#include "util.h"
#include "../../vendor/minhook/include/MinHook.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

DeviceOriginals g_deviceOriginals;
ContextOriginals g_immediateOriginals;
ContextOriginals g_deferredOriginals;
void** g_immediateVtable = nullptr;
bool g_installed = false;
bool g_installPoisoned = false;
atfix::mutex g_installMutex;

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

// The pre-UI pass's set: the four draw slots a first-draw anchor fires from.
// The slots and the originals are this file's business; the detours themselves
// are the engine's, and arrive through ScenePolicy::drawDetours. The frame map
// rides the same detours so only one set exists on the vtable.
//
// Built at install time rather than declared, because an engine without a draw
// anchor supplies four nulls and this set is then not installed at all.
ContextHookSpec preUiHooks[4] = {
  { 12, nullptr, offsetof(ContextOriginals, drawIndexed),
    "DrawIndexed(pre-UI)" },
  { 13, nullptr, offsetof(ContextOriginals, draw), "Draw(pre-UI)" },
  { 20, nullptr, offsetof(ContextOriginals, drawIndexedInstanced),
    "DrawIndexedInstanced(pre-UI)" },
  { 21, nullptr, offsetof(ContextOriginals, drawInstanced),
    "DrawInstanced(pre-UI)" },
};

#undef ORIGINAL_AT

// Add one context vtable to the central transaction. Nothing is enabled here:
// every requested device/context target must first be created successfully, so
// a later failure can remove the whole owned set while it is still inert.
bool addContextVtable(void** vtable, ContextOriginals& originals,
                      const ContextHookSpec* specs, int count,
                      const char* which, HookTransaction& transaction) {
  auto* base = reinterpret_cast<uint8_t*>(&originals);
  for (int i = 0; i < count; ++i) {
    void* fn = vtable[specs[i].slot];
    void** slot = reinterpret_cast<void**>(base + specs[i].originalOffset);
    if (!transaction.create(fn, specs[i].detour, slot)) {
      const HookTransactionFailure& failure = transaction.failure();
      log("D3D11HOOKS: transaction ", hookTransactionStageName(failure.stage),
          " failed at ", which, "::", specs[i].name,
          failure.status ? " status=" : "",
          failure.status
            ? MH_StatusToString(static_cast<MH_STATUS>(failure.status)) : "");
      return false;
    }
  }
  return true;
}

void clearHookPublications() {
  g_deviceOriginals = {};
  g_immediateOriginals = {};
  g_deferredOriginals = {};
  g_immediateVtable = nullptr;
}

void logRollbackFailure(const HookTransaction& transaction) {
  const HookTransactionFailure& failure = transaction.rollbackFailure();
  log("D3D11HOOKS: ROLLBACK INCOMPLETE stage=",
      hookTransactionStageName(failure.stage), " target=", failure.target,
      failure.status ? " status=" : "",
      failure.status
        ? MH_StatusToString(static_cast<MH_STATUS>(failure.status)) : "",
      "; refusing every later install attempt because hook ownership is now"
      " uncertain");
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
  if (!device)
    return;
  std::lock_guard<atfix::mutex> installLock(g_installMutex);
  if (g_installed || g_installPoisoned)
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
      !anisotropyLevel() && !smaaPreUiEnabled() && !ssaaActive() &&
      !frameMapEnabled() && !sharpenEnabled())
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
  // The pre-UI anchor fires from the draw detours in this set, so a plain SMAA
  // session needs them too -- not just a trace run.
  // The draw slots belong to the pre-UI pass; the frame map rides along on the
  // same detours.
  //
  // ASKED OF THE ENGINE, not of the resolution fix. This used to decline the
  // set whenever `highRes.rasterCorrection || ssaaConfigured()` was true, and
  // then log the decision in KTGL's words. On Ayesha the raster correction is
  // on in every session, so the line printed "supersampling is on" while
  // supersampling was off -- and it was describing a set Ayesha does not want
  // for a reason that has nothing to do with why it does not want it.
  const bool wantsDrawSet =
    (scenePolicy().preUiAtFirstDraw() &&
     (smaaPreUiEnabled() || sharpenEnabled())) || frameMapEnabled();
  if (wantsDrawSet && (highRes.rasterCorrection || ssaaConfigured()))
    log("Pre-UI anchor: riding the raster correction's draw detours this run"
        " -- one vtable slot cannot hold two, and the anchor is reached through"
        " ScenePolicy::afterDraw instead. The pass still runs.");
  else if (wantsDrawSet && scenePolicy().drawDetours[0]) {
    for (int i = 0; i < 4; ++i)
      preUiHooks[i].detour = scenePolicy().drawDetours[i];
    append(preUiHooks, 4);
  }
  // Sharpening rides the same bind detour: it needs the pre-UI anchor, and the
  // anchor is fed from OMSetRenderTargets. Leaving it out here is how a
  // sharpening-only session installed nothing and reported nothing.
  if (smaaPreUiEnabled() || ssaaActive() || frameMapEnabled() ||
      sharpenEnabled())
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

  // Acquire the deferred vtable before asking MinHook to create anything. A
  // device that cannot supply it must leave no device hook behind either.
  ID3D11DeviceContext* deferred = nullptr;
  void** immediateVtable = specCount
    ? *reinterpret_cast<void***>(context) : nullptr;
  void** deferredVtable = nullptr;
  bool distinctVtables = false;
  if (specCount) {
    const HRESULT hr = device->CreateDeferredContext(0, &deferred);
    if (FAILED(hr) || !deferred) {
      log("D3D11HOOKS: CreateDeferredContext failed (hr=0x", std::hex,
          uint32_t(hr), std::dec, "); the context hooks would miss every draw"
          " this engine issues, so installing nothing");
      if (owned)
        owned->Release();
      return;
    }
    deferredVtable = *reinterpret_cast<void***>(deferred);
    distinctVtables = deferredVtable != immediateVtable;
    log("D3D11HOOKS: context vtables immediate=",
        reinterpret_cast<void*>(immediateVtable),
        " deferred=", reinterpret_cast<void*>(deferredVtable),
        distinctVtables ? " (distinct, both transactional)"
                        : " (shared, one transactional set)");
  }

  clearHookPublications();
  g_immediateVtable = immediateVtable;
  HookTransaction transaction;
  auto decline = [&]() {
    const HookTransactionFailure& failure = transaction.failure();
    log("D3D11HOOKS: transaction declined stage=",
        hookTransactionStageName(failure.stage), " target=", failure.target,
        failure.status ? " status=" : "",
        failure.status
          ? MH_StatusToString(static_cast<MH_STATUS>(failure.status)) : "");
    if (!transaction.rollback()) {
      g_installPoisoned = true;
      logRollbackFailure(transaction);
    } else {
      clearHookPublications();
      log("D3D11HOOKS: transaction rolled back completely; a later device may"
          " retry");
    }
  };

  // Device slot 5. Wanted by the resolution fix and by the census alike, so it
  // goes in whenever either is on, and its failure is fatal to both.
  if (highRes.createTexture2D) {
    if (!transaction.create(deviceVtable[5],
          reinterpret_cast<void*>(&hookedCreateTexture2D),
          reinterpret_cast<void**>(&g_deviceOriginals.createTexture2D))) {
      decline();
      if (deferred)
        deferred->Release();
      if (owned)
        owned->Release();
      return;
    }
  }

  // Device slot 23, CreateSamplerState. Cheap and unconditional when on: the
  // upgrade is a descriptor rewrite at creation time, and sampler states are
  // created a handful of times per session, not per frame.
  if (anisotropyLevel()) {
    if (!transaction.create(deviceVtable[23],
          reinterpret_cast<void*>(&hookedCreateSamplerState),
          reinterpret_cast<void**>(&g_deviceOriginals.createSamplerState))) {
      decline();
      if (deferred)
        deferred->Release();
      if (owned)
        owned->Release();
      return;
    }
  }

  int hookedVtables = 0;
  if (specCount) {
    if (!addContextVtable(immediateVtable, g_immediateOriginals, specs,
                          specCount, "immediate", transaction)) {
      decline();
      deferred->Release();
      if (owned)
        owned->Release();
      return;
    }
    ++hookedVtables;

    if (distinctVtables) {
      if (!addContextVtable(deferredVtable, g_deferredOriginals, specs,
                            specCount, "deferred", transaction)) {
        decline();
        deferred->Release();
        if (owned)
          owned->Release();
        return;
      }
      ++hookedVtables;
    } else {
      // One vtable serves both; copying is safe because its hooks were created
      // by this transaction, not inferred from an ALREADY_CREATED response.
      g_deferredOriginals = g_immediateOriginals;
    }
  }

  if (!transaction.enableAll()) {
    decline();
    if (deferred)
      deferred->Release();
    if (owned)
      owned->Release();
    return;
  }
  transaction.commit();
  if (deferred)
    deferred->Release();

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
  // NOT on the engine that has its own pre-UI anchor. That anchor identifies
  // the moment structurally and never asks for a main render size, so on Escha
  // & Logy and Shallie this warning was simply false -- it printed in every
  // session of a working feature, which is worse than not printing at all.
  if (!highRes.rasterCorrection && scenePolicy().needsMainRenderSize() &&
      (smaaPreUiEnabled() || ssaaConfigured()))
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
