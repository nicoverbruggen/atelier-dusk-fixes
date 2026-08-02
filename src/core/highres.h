// SPDX-License-Identifier: MIT
#pragma once

// High-resolution rendering, and the census that measured the need for it.
//
// The two live in one file because they are one subsystem: both hang off the
// same `ID3D11Device::CreateTexture2D` hook, and MinHook allows exactly one
// hook per target, so they could not be separate installs even if the split
// were desirable. It is not -- the census is how the fix is verified, and
// running them together is what makes "did this actually resize everything"
// answerable in a single session.
//
// THE DEFECT, measured rather than assumed (WORK_DOC.md, "Measured: Ayesha has
// the old-Arland defect"). Ayesha takes any resolution from its own
// Setting.ini and creates a swap chain and matching depth target at that size,
// but every target the scene is actually drawn into is created at a hard-coded
// 1920x1080. The scene is therefore rendered at 1080p and scaled up, and
// choosing 1440p buys a larger window and no more detail.
//
// THE FIX is the one TellowKrinkle established for this engine family and the
// Arland project refined: learn the main render size from the first
// depth-stencil target the game creates, then give every later hard-coded
// target the same size, and correct the hard-coded viewport and scissor to
// match. It is a pure D3D11-layer correction -- no game addresses, no
// prologues, nothing engine-specific -- which is why it lives in core rather
// than in src/engines/phyre despite currently applying only to Ayesha.
#include "d3d11_hooks.h"

namespace atfix {

// ---- wiring for d3d11_hooks.cpp -------------------------------------------
//
// This module owns the detours below and the policy inside them; it does not
// own the vtables they are installed into. See d3d11_hooks.h for why that split
// exists. Nothing outside d3d11_hooks.cpp should reference this section.

// What this module needs hooked, and the side effect of resolving its feature
// flags. Called once, before anything is installed.
//
// `createTexture2D` is wanted by the resolution fix and the census alike -- the
// census reports from inside that same detour. `rasterCorrection` is the fix
// only: a diagnostic that changed raster state would not be a diagnostic.
struct HighResWants {
  bool createTexture2D;
  bool rasterCorrection;
};
HighResWants highResResolveWants();

// Which context is the immediate one. The raster correction keeps its state per
// context and decides by pointer compare, so it has to be told.
void highResNoteImmediateContext(ID3D11DeviceContext* context);

HRESULT STDMETHODCALLTYPE hookedCreateTexture2D(
  ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
  ID3D11Texture2D**);
void STDMETHODCALLTYPE hookedRSSetViewports(
  ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
void STDMETHODCALLTYPE hookedRSSetScissorRects(
  ID3D11DeviceContext*, UINT, const D3D11_RECT*);
void STDMETHODCALLTYPE hookedDraw(ID3D11DeviceContext*, UINT, UINT);
void STDMETHODCALLTYPE hookedDrawIndexed(
  ID3D11DeviceContext*, UINT, UINT, INT);
void STDMETHODCALLTYPE hookedDrawInstanced(
  ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
void STDMETHODCALLTYPE hookedDrawIndexedInstanced(
  ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

// Called from the hooked Present. Emits the census summary, which carries the
// number of creations seen: a census whose only output is "nothing found" is
// indistinguishable from one that never installed.
void highResFrameTick();

// The size the scene renders at, learned from the first main target the game
// creates. False until it has created one.
//
// Exposed because the MSAA module's scene-target test needs it, and that test
// lives in the engine module rather than in core (see msaa.h on MsaaSceneTest).
// This reports a fact; deciding what it means is the caller's business.
bool highResMainSize(unsigned int* width, unsigned int* height);

// The size the scene targets are actually created at: the main render size
// with any supersampling factor already applied. False until a main target has
// been seen.
//
// This exists because that size was briefly written down in two places -- the
// resize in hookedCreateTexture2D and the MSAA scene test -- and they
// disagreed the moment supersampling was enabled. The scene targets became
// larger than the main size, the scene test still matched on the main size, and
// MSAA declined every bind for a whole session. Two features that each worked
// alone, and enabling one turned the other off. One definition, used by both.
bool highResSceneSize(unsigned int* width, unsigned int* height);

// Records the size the swap chain was actually created at. Logged once, and
// independently of whether either feature is enabled -- one line naming the
// present resolution is worth having in every log.
void noteSwapChainSize(unsigned int width, unsigned int height,
                       unsigned int format, unsigned int refreshNumerator,
                       unsigned int refreshDenominator, bool windowed);

}  // namespace atfix
