// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

// Multisample anti-aliasing for the Dusk games.
//
// WHY THIS IS NOT A SAMPLE-COUNT BUMP. The first version of this feature raised
// `SampleDesc.Count` on the targets the engine already created multisampled,
// on the reading that Ayesha "already renders 4x MSAA and just needs turning
// up". That reading was wrong, and it was wrong in an instructive way: it came
// from the target census, which reports the targets the game CREATES. Creation
// is not use. Draw-time instrumentation settled it -- over 7200 frames,
// 117245 sampled draws, `drawsToMsaa=0` and `maxBoundSamples=0`. Not one draw
// ever landed on a multisampled target.
//
// The engine does create six of them at startup (colour+depth pairs at
// 1920x1080, 1728x972 and 1536x864, exactly 100/90/80%) and renders into none.
// Static analysis says why. Ayesha's texture-creation wrapper at 0x559600
// takes an anti-aliasing LEVEL INDEX as its seventh argument, looks the sample
// count up in a table at RVA 0xfe8418 holding `1, 2, 4, 0`, and stores it into
// the descriptor:
//
//   0x55965c  movsxd rcx, dword ptr [rbp+0x4f]        ; arg7 = AA level
//   0x559672  mov eax, dword ptr [rax + rcx*4]        ; rax = 0xfe8418
//   0x559675  mov dword ptr [rbp-0x65], eax           ; -> SampleDesc.Count
//
// Six of the wrapper's eight call sites pass a literal zero. Only two forward a
// variable, and that chain resolves to a renderer field (`renderer+0x305c`)
// which drives the dead allocations. The scene's own colour and depth pair
// comes from a hardcoded-zero site, and no ini key, registry value or command
// line reaches any function on those paths -- Ayesha's only config reader
// handles ScreenWidth/ScreenHeight/fullscreen and nothing else. The engine's
// shipped anti-aliasing is FXAA, inside its tonemap pass.
//
// So there is no engine switch to flip, and MSAA has to be built outside the
// renderer. See WORK_DOC.md, "MSAA: why the engine cannot be asked".
//
// THE MECHANISM is the twin-resource one, from TellowKrinkle's rendering work
// and the Arland project's adaptation of it (`src/sync_fix.cpp`). The game goes
// on owning its single-sample colour and depth targets -- the "hosts" -- and
// nothing it creates changes shape. The mod attaches a multisample "twin" to
// each host as private data, substitutes the twins when the game binds that
// pair as its render targets, and lands the twin back into its host before
// anything reads the host. Every resource the game itself created keeps the
// size, format and sample count it asked for, so no shader, no view and no
// copy the game performs sees anything unexpected.
//
// This is engine-agnostic D3D11 with no mapped addresses, which is the reason
// it lives in core: the same implementation is what Escha & Logy and Shallie
// need, and neither of those has any anti-aliasing today.
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace atfix {

// The requested sample count: 0 or 1 means off, otherwise 2, 4 or 8. Resolved
// once from the capability matrix and the environment. Never returns a count
// the device cannot provide -- an unsupported request walks down through lower
// counts, as the Arland implementation does, so an over-ambitious setting
// degrades instead of failing.
unsigned int msaaSamples();

// Whether MSAA is on and at least one twin pair has actually been created.
// "Configured" and "engaged" are different questions and the log answers both
// separately: the whole reason this feature needed rewriting is that the old
// one reported itself active while changing nothing on screen.
bool msaaActive();

// Learn the device. Called once from the high-resolution module, which owns the
// device vtable; this module never hooks anything itself, for the reason
// supersample.h gives -- one vtable hooked from two places is how a disable
// race gets written by accident.
void msaaInitialize(ID3D11Device* device);

// Which colour+depth pair is the scene pass.
//
// THIS IS THE ENGINE-SPECIFIC PART OF MSAA, and it is a callback rather than a
// rule in this file because it is the one piece that genuinely does not
// generalize. Everything else here is D3D11 mechanism that holds for any game:
// build a twin, substitute it at bind, land it before a read. But "which of the
// binds a frame issues is the one carrying the 3D scene" is a property of a
// renderer, and the prior art proves it varies -- TellowKrinkle identifies it by
// counting indexed draws per target, the Arland project by matching the main
// render size and a BGRA view format. Guessing wrong does not fail loudly: it
// multisamples a shadow map or a UI layer and leaves the scene untouched.
//
// So core declines every bind until an engine module registers a test, and says
// so in the log rather than silently doing nothing. `src/engines/phyre/scene_target.cpp`
// registers Ayesha's. Escha & Logy and Shallie have none, which is the honest
// state: their renderer has never been censused, so nothing is known about what
// they bind.
//
// Core still applies its own structural checks on top -- single-sample, one mip,
// one array slice -- because those are about whether a twin can exist at all,
// not about which surface is interesting.
using MsaaSceneTest = bool (*)(const D3D11_TEXTURE2D_DESC& color,
                               const D3D11_TEXTURE2D_DESC& depth);
void msaaSetSceneTest(MsaaSceneTest test);

// How to set a context's render targets without re-entering our own hook.
//
// This exists because of a hazard the Arland implementation lives with and this
// one should not. `ResolveSubresource` reads its source, and a twin that is
// still bound as a render target is a read-after-write conflict: the debug
// layer rejects it and a translation layer is free to drop the call. Every
// resolve here therefore drops the bindings around itself and puts them back,
// and only the module that owns the context vtable can do that without
// recursing into its own OMSetRenderTargets detour.
//
// Saving and restoring rather than merely unbinding is not defensive padding.
// Two of the resolve points -- a shader-resource bind and a copy -- happen in
// the middle of a pass with the scene targets legitimately bound, and an
// unbind that was not put back would leave every following draw in that pass
// writing nowhere.
// Track the scene/UI boundary for the pre-UI SMAA pass, and fire it when the
// engine stops rendering into the scene target. Independent of whether MSAA is
// enabled: SMAA needs the same "which bind is the scene" answer, and this
// module already owns both that test and the bind hook.
void msaaNoteSceneBoundary(ID3D11DeviceContext* context, unsigned int numViews,
                           ID3D11RenderTargetView* const* views,
                           ID3D11DepthStencilView* depth);

void msaaSetTargetBinder(
  void (*bind)(ID3D11DeviceContext*, unsigned int,
               ID3D11RenderTargetView* const*, ID3D11DepthStencilView*));

// ---- the interception points ----------------------------------------------
//
// Each of these is called from the corresponding hook in highres.cpp. They are
// separate entry points rather than one "handle everything" call because the
// resolve discipline is the entire correctness argument of this feature and it
// is easier to audit when each read path names itself.

// Substitute the twins for a bind of the scene colour+depth pair. Returns true
// when it substituted, having written the twin views into `rtvOut`/`dsvOut`
// (caller owns those references). Returns false for every other bind, which is
// the overwhelming majority, and the caller then forwards unchanged.
//
// Call this BEFORE forwarding to the real OMSetRenderTargets. It also sets
// aside whatever pair was bound until now, for msaaResolveReplaced to land.
bool msaaSubstituteTargets(ID3D11DeviceContext* context,
                           unsigned int numViews,
                           ID3D11RenderTargetView* const* views,
                           ID3D11DepthStencilView* depth,
                           ID3D11RenderTargetView** rtvOut,
                           ID3D11DepthStencilView** dsvOut);

// Land the pair the bind just replaced. Called from the bind detours after
// msaaSubstituteTargets and before forwarding. The order relative to the
// forward is not what makes the resolve legal -- the resolve drops and
// restores the render-target bindings around itself (see msaaSetTargetBinder),
// so it is legal on either side. Before is chosen so the game's own bind is
// the last word on the context's state, rather than this module's restore.
void msaaResolveReplaced(ID3D11DeviceContext* context);

// Land any dirty twin among these shader resources back into its host, before
// the game samples it. This is the read path that matters most, because the
// post-processing chain samples the scene colour it was just rendered into.
//
// It also counts SRV binds of a twinned DEPTH host, which are not resolved and
// cannot be -- see msaa.cpp on resolveColor. That count is the one open
// question this feature ships with.
void msaaResolveShaderResources(ID3D11DeviceContext* context,
                                unsigned int numViews,
                                ID3D11ShaderResourceView* const* views);

// Land a dirty twin back into `source` before it is copied out of.
void msaaResolveCopySource(ID3D11DeviceContext* context,
                           ID3D11Resource* source);

// Land whatever is currently bound, because a recorded command list is about to
// become someone else's problem. Ayesha draws on a deferred context, so this is
// not an edge case here -- it is the normal end of every frame's scene work.
void msaaResolveBeforeFinish(ID3D11DeviceContext* context);

// Force `MultisampleEnable` on a rasterizer state the game creates. Without
// this the twins are multisampled and the rasteriser still emits single-sample
// coverage, which produces the exact symptom this feature exists to remove
// while every diagnostic reports success.
void msaaAdjustRasterizerState(D3D11_RASTERIZER_DESC* desc);

// A last resolve before the frame is shown, against the IMMEDIATE context.
//
// Do not read this as a safety net for Ayesha: it very likely does nothing
// there. Ayesha records its scene on a deferred context, and FinishCommandList
// resolves and clears that context's markers before the list is ever replayed,
// so the immediate context's slots are normally already empty by Present. This
// earns its place for a renderer that draws on the immediate context -- which
// is the case the Arland implementation was written against and which Escha &
// Logy and Shallie have not been measured for.
void msaaResolveBeforePresent(IDXGISwapChain* swapChain);

// Per-frame counters for the log. Reports what actually happened -- twins
// created, binds substituted, resolves performed -- because "did MSAA engage"
// must never again be answerable only by inference.
void msaaFrameTick();

// ---- wiring for d3d11_hooks.cpp -------------------------------------------
//
// This module owns these detours and everything they decide; it does not own
// the vtables they are installed into. See d3d11_hooks.h. Nothing outside
// d3d11_hooks.cpp should reference this section.
//
// Every one of them forwards unconditionally and asks this module what else to
// do, so none changes behaviour when MSAA is off -- the entry points above all
// return immediately on a zero sample count.
void STDMETHODCALLTYPE hookedOMSetRenderTargets(
  ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
  ID3D11DepthStencilView*);
void STDMETHODCALLTYPE hookedPSSetShaderResources(
  ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
void STDMETHODCALLTYPE hookedCopyResource(
  ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
void STDMETHODCALLTYPE hookedCopySubresourceRegion(
  ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
  ID3D11Resource*, UINT, const D3D11_BOX*);
HRESULT STDMETHODCALLTYPE hookedFinishCommandList(
  ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
HRESULT STDMETHODCALLTYPE hookedCreateRasterizerState(
  ID3D11Device*, const D3D11_RASTERIZER_DESC*, ID3D11RasterizerState**);
// The clears redirect to the twin. The game clears the view it created, which
// is the host; the twin is what is bound and drawn into. Without these it is
// never cleared and every frame draws on top of the last.
void STDMETHODCALLTYPE hookedExecuteCommandList(
  ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
void STDMETHODCALLTYPE hookedClearRenderTargetView(
  ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
void STDMETHODCALLTYPE hookedClearDepthStencilView(
  ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
void STDMETHODCALLTYPE hookedOMSetRenderTargetsAndUnorderedAccessViews(
  ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
  ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*,
  const UINT*);

// The binder this module registers with itself through msaaSetTargetBinder.
// It forwards through the trampoline rather than the public method, so a
// resolve can drop and restore render targets without re-entering the
// OMSetRenderTargets detour above.
void msaaBindTargets(ID3D11DeviceContext* context, unsigned int numViews,
                     ID3D11RenderTargetView* const* views,
                     ID3D11DepthStencilView* depth);

}  // namespace atfix
