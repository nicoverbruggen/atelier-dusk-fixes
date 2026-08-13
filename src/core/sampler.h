// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Anisotropic texture filtering.
//
// Some scene samplers ask for plain trilinear filtering. On ground planes and
// walls viewed at an angle, anisotropic filtering can preserve more texture
// detail by spending samples along the stretched direction.
//
// The mechanism is the Arland project's, unchanged (`src/sync_fix.cpp`,
// `ID3D11Device_CreateSamplerState`): intercept sampler creation and rewrite
// the filter. Pure D3D11 with no mapped addresses and no engine knowledge,
// which is why the implementation lives in core rather than needing a port per
// engine. The capability matrix nevertheless enables it only for Ayesha; the
// same code is deliberately inert on both KTGL games.
//
// WHAT IS DELIBERATELY NOT UPGRADED. Anything at or above
// D3D11_FILTER_ANISOTROPIC (0x55) already asked for what we would give it, and
// the comparison, minimum and maximum filters (>= 0x80) are a different thing
// entirely -- shadow-map percentage-closer filtering lives there, and turning
// that into an anisotropic sample is not an improvement, it is a different
// operation. The bound is therefore the basic point/linear block, 0x00..0x15,
// exactly as Arland has it.
//
// THE POINT-SAMPLER QUESTION. That bound includes
// D3D11_FILTER_MIN_MAG_MIP_POINT, which is enum zero -- so every point sampler
// is upgraded too. Ayesha is runtime-validated and reported `fromPoint=0`, so
// the question is moot there. It remains unresolved on both KTGL games: point
// sampling is *correct* for lookup textures, and filtering a colour-grading
// LUT, a gradient ramp or a dither table smears the very thing it encodes. That
// is one reason Escha & Logy and Shallie mark this feature Unsupported.
//
// The Ayesha path still counts what it upgrades, by kind, and says so. Its
// validated run found no point samplers; `DUSK_ANISO_KEEP_POINT=1` remains a
// narrow comparison switch if that ever changes.
namespace atfix {

// The anisotropy level: 0 or 1 means off, otherwise 2, 4, 8 or 16. Resolved
// once from the capability matrix, the environment and the ini.
unsigned int anisotropyLevel();

// Rewrite a sampler descriptor in place, if it is one we upgrade. Returns true
// when it changed something.
bool samplerUpgrade(D3D11_SAMPLER_DESC* desc);

// One line, once, naming what was actually upgraded. Silence would leave "the
// filtering is unchanged" and "the hook never ran" indistinguishable, which is
// the failure this project keeps having to design against.
void samplerReport();

// ---- wiring for d3d11_hooks.cpp -------------------------------------------
HRESULT STDMETHODCALLTYPE hookedCreateSamplerState(
  ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);

}  // namespace atfix
