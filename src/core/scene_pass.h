// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Which render-target bind is the 3D scene, and when the engine leaves it.
//
// Two features need that answer and neither can produce it. The pre-UI SMAA
// pass needs to know the moment the scene is finished and the interface is not
// yet drawn, so it has something to antialias that is not the HUD.
// Supersampling needs to recognise the scene colour surface later, when the
// composite samples it, so it can substitute its own downscale for the
// engine's. Both questions are answered from the same place: the render-target
// bind, which this module detours.
//
// The verdict itself is engine-specific and is not in this file. Core declines
// every bind until an engine module registers a test, and says so in the log
// rather than silently doing nothing. `src/engines/phyre/scene_target.cpp`
// registers Ayesha's. Escha & Logy and Shallie have none, which is the honest
// state: their renderer has never been censused, so nothing is known about what
// they bind.
//
// This module used to be msaa.cpp, and the multisampling it was written around
// is gone. Do not put it back without reading the reasoning first: MSAA cannot
// reach what actually aliases in these games -- sub-pixel detail inside
// textures and alpha-tested trim, which only supersampling resolves -- and the
// Arland project removed its own twin implementation in the same week for the
// same reason, after it turned a conversation backdrop solid black. What is
// left here is the part both surviving features were leaning on.
namespace atfix {

// Which colour+depth pair is the scene pass.
//
// A callback rather than a rule in this file because it is the one piece that
// genuinely does not generalize. "Which of the binds a frame issues carries the
// 3D scene" is a property of a renderer, and the prior art proves it varies --
// TellowKrinkle identifies it by counting indexed draws per target, the Arland
// project by matching the main render size and a BGRA view format. Guessing
// wrong does not fail loudly: it antialiases a shadow map or a UI layer and
// leaves the scene untouched.
using SceneTargetTest = bool (*)(const D3D11_TEXTURE2D_DESC& color,
                                 const D3D11_TEXTURE2D_DESC& depth);
void scenePassSetTest(SceneTargetTest test);

// Track the scene/UI boundary and fire the pre-UI SMAA pass when the engine
// stops rendering into the scene target, then tag the scene colour host for
// supersampling.
//
// Called from the bind detours below rather than from either feature, because
// this is the only place that knows which colour target the engine's scene test
// accepted. Gating it on the SMAA switch alone once made supersampling depend
// on an unrelated checkbox, silently.
void scenePassFrameTick();

// The scene colour surface this engine's test accepted, or null. Exposed so the
// Glow anchor can check it against the texture the composite actually samples:
// SMAA reports firing on the right surface and has no visible effect, and a
// mismatch here would explain that in one line.
void* scenePassAcceptedSurface();

void scenePassNoteBoundary(ID3D11DeviceContext* context, unsigned int numViews,
                           ID3D11RenderTargetView* const* views,
                           ID3D11DepthStencilView* depth);

// ---- wiring for d3d11_hooks.cpp -------------------------------------------
//
// This module owns these detours and everything they decide; it does not own
// the vtables they are installed into. See d3d11_hooks.h. Nothing outside
// d3d11_hooks.cpp should reference this section.
//
// Every one of them forwards unconditionally, so none changes behaviour when
// both features that consume the boundary are off.
void STDMETHODCALLTYPE hookedOMSetRenderTargets(
  ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
  ID3D11DepthStencilView*);
void STDMETHODCALLTYPE hookedPSSetShaderResources(
  ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
HRESULT STDMETHODCALLTYPE hookedFinishCommandList(
  ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
// The other way to bind render targets. Nothing in these engines is known to
// use it, but "known" here means "never looked", and a bind arriving this way
// would cross the scene/UI boundary without either feature noticing.
void STDMETHODCALLTYPE hookedOMSetRenderTargetsAndUnorderedAccessViews(
  ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
  ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*,
  const UINT*);

}  // namespace atfix
