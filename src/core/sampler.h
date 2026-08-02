// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Anisotropic texture filtering.
//
// These games ask for plain trilinear filtering and get exactly that, which is
// what makes ground planes and walls smear into mush the moment they are viewed
// at an angle -- the further from perpendicular a surface is, the fewer texels
// a trilinear sample can afford to average, and the detail goes with them.
// Anisotropic filtering spends more samples along the direction the surface is
// stretched in, and on any GPU these ports can run on it is close to free.
//
// The mechanism is the Arland project's, unchanged (`src/sync_fix.cpp`,
// `ID3D11Device_CreateSamplerState`): intercept sampler creation and rewrite
// the filter. Pure D3D11 with no mapped addresses and no engine knowledge,
// which is why it is in core and why it covers all three Dusk games at once
// rather than needing a port per engine.
//
// WHAT IS DELIBERATELY NOT UPGRADED. Anything at or above
// D3D11_FILTER_ANISOTROPIC (0x55) already asked for what we would give it, and
// the comparison, minimum and maximum filters (>= 0x80) are a different thing
// entirely -- shadow-map percentage-closer filtering lives there, and turning
// that into an anisotropic sample is not an improvement, it is a different
// operation. The bound is therefore the basic point/linear block, 0x00..0x15,
// exactly as Arland has it.
//
// THE POINT-SAMPLER QUESTION, which is open. That bound includes
// D3D11_FILTER_MIN_MAG_MIP_POINT, which is enum zero -- so every point sampler
// is upgraded too. Arland ships that and gets away with it. It is not obviously
// safe here: point sampling is *correct* for lookup textures, and filtering a
// colour-grading LUT, a gradient ramp or a dither table smears the very thing
// it encodes. This is flagged as needing validation against a
// scene with tonemapping and post-processing running, not just a menu.
//
// So this counts what it upgrades, by kind, and says so. If a run reports no
// point samplers at all the question is moot for that game; if it reports many
// and something looks wrong, `DUSK_ANISO_KEEP_POINT=1` leaves them alone and
// the difference is one run rather than an argument.
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
