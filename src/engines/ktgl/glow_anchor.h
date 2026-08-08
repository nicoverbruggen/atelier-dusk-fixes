// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include "ktgl.h"

// The Glow composite: where a pre-UI antialiasing pass would go on Escha & Logy
// and Shallie, and how to know when the engine is about to draw it.
//
// WHY AN ANCHOR IS NEEDED AT ALL. SMAA already runs on these games at Present,
// over the finished frame, which softens the interface along with everything
// else -- which is why it is opt-in there rather than on by default. Moving it
// before the interface is drawn is the whole point, and that requires a moment
// in the frame that is provably after the scene and before the UI. Ayesha gets
// that from a scene-target test; these games have none, and the census that
// would build one has never been run.
//
// THE ANCHOR. `PostEffectGlow.kps` holds seventeen pixel shaders. Sixteen bind
// exactly one texture. Container #15 is the only one binding TWO, and it names
// them in its RDEF: `smplScene_Tex` and `smplGlowTargetsY3_Tex`. That is the
// terminal composite of the bloom chain -- the pass that puts the blurred glow
// back over the scene -- and it is identical in shape across both games.
//
// IDENTIFIED BY CHECKSUM AND LENGTH, not by those names at runtime: the names
// are how the shader was FOUND, in the shipped pack, offline. At runtime all
// that arrives is bytecode, and the DXBC container's own checksum plus its exact
// length is the cheapest exact test. The names are recorded in the table's
// comment so the next person can re-derive the row rather than trust it.
//
// THE FAILURE TO AVOID is documented and specific. The third-party mod that
// pioneered this anchor registered Shallie's fingerprint and never fired: its
// arming test compared against shader slots that only its Arland code path
// populates. It logged success throughout. So this module's first job is not to
// inject anything -- it is to report whether the anchor fires, how often, and in
// which frames.
namespace atfix {

// Installs `DUSK_GLOW_TRACE`, which identifies the composite and reports when it
// is bound. Changes nothing. Returns true when the hooks went in.
bool installGlowTrace(const KtglGame& game);

// Wiring for d3d11_hooks.cpp. Device slot 15 to recognise the shader when it is
// created, context slot 9 to notice when it is bound.
bool glowTraceEnabled();

// Called from the hooked Present, for the periodic tally.
void glowTraceFrameTick();

// Fed from the pre-UI feature's draw detours.
void glowTraceNoteDraw(ID3D11DeviceContext* self);

HRESULT STDMETHODCALLTYPE hookedCreatePixelShader(
  ID3D11Device* self, const void* bytecode, SIZE_T length,
  ID3D11ClassLinkage* linkage, ID3D11PixelShader** out);

void STDMETHODCALLTYPE hookedPSSetShader(
  ID3D11DeviceContext* self, ID3D11PixelShader* shader,
  ID3D11ClassInstance* const* instances, UINT numInstances);

}  // namespace atfix
