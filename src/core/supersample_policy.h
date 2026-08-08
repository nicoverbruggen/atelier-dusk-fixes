// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

// The questions supersampling asks that the two engines answer differently.
//
// WHY THIS EXISTS. Supersampling is one feature reached by two opposite routes.
// Ayesha pins its scene targets at 1920x1080, so the mod enlarges them and owns
// the resolve. KTGL sizes everything from its own ini, so the mod only has to
// stop the swap chain from following it up -- the present clamp. The box filter
// in the middle is the same work in both cases; everything around it is not.
//
// Before this header the difference was nine `if (ssaaPresentClampEnabled())`
// branches spread through supersample.cpp, and one of them was read wrongly: a
// change made for Ayesha shipped a call that is null on KTGL and crashed both
// of those games on boot. The branches are now one table, answered
// once per engine, in that engine's own directory.
//
// PULLED, NOT PUSHED, and that is the whole reason this is a lazy accessor
// rather than a register-at-init call. The clamp's first and only important
// customer is the DXGI CreateSwapChain hook, which runs before any engine module
// has initialized. A registration model would have had the policy arrive after
// the call that needed it, and the failure would have been silent: no clamp, no
// error, a picture that looks like a feature nobody switched on. Resolving
// lazily from currentEngine() -- which fingerprints the running executable and
// needs no initialization -- is what the code did before this split and is what
// it still does.
namespace atfix {

struct SsaaPolicy {
  // Is this engine's supersampling route on at all? Ayesha asks whether the
  // factor is above 100; KTGL asks whether the present clamp is engaged.
  bool (*routeActive)();

  // The size the composite resamples the scene DOWN TO, for the substitution.
  // Ayesha takes the main render size. KTGL takes the swap-chain size, because
  // the clamp forced the back buffer to it and the main render size is never
  // learned on that engine at all.
  bool (*substitutionDestSize)(unsigned int* width, unsigned int* height);

  // The size the composite's viewport SHOULD BE, which is a different question
  // with a different answer on both engines. Ayesha reads the colour target that
  // is actually bound, because a size cached at startup is wrong the moment
  // anything resizes the buffers. KTGL cannot: the clamp resized the back buffer
  // behind the engine's back, so the ini's display size is the authority.
  bool (*compositeViewportSize)(ID3D11DeviceContext* context,
                                unsigned int* width, unsigned int* height);

  // Clamp a requested back-buffer size down to the display size, and resize a
  // windowed swap chain's output window to match. Both no-ops on Ayesha, which
  // has no clamp. Never null -- the Ayesha policy supplies empty functions, so
  // no caller has to test before calling.
  void (*clampPresentSize)(UINT* width, UINT* height, const char* where);
  void (*fitOutputWindow)(const DXGI_SWAP_CHAIN_DESC* desc);

  // Does this route clamp the swap chain at all? The DXGI proxy installs its
  // ResizeBuffers and ResizeTarget hooks only when something is going to clamp
  // through them, so an ordinary session has neither.
  bool clampsPresentSize;

  // Must a candidate scene colour host carry the scene-host tag?
  //
  // Ayesha's scene test supplies that tag and the answer is yes. KTGL has no
  // scene test, and what stands in for it is stronger than it sounds: the
  // substitution only runs while the swap chain's back buffer is bound as a
  // colour target, and the only thing sampled at that moment which is a colour
  // target larger than the back buffer is the frame being resolved into it.
  bool requiresSceneHostTag;

  // Does this route depend on the high-resolution fix? Ayesha's does and says so
  // when it is off. KTGL enlarges from its own ini, where that fix is correctly
  // unsupported, and saying "supersampling is inactive" there would be the
  // opposite of true -- it was once printed three lines above a line reporting
  // the feature engaged.
  bool requiresHighRes;
};

// The policy for the engine in this process. Resolved on first use and cached.
// Returns a policy whose routeActive() is false when no engine matched, so every
// caller can use it without a null test.
//
// Defined in engine.cpp, which is the dispatch layer and the one file entitled
// to name both engine modules.
const SsaaPolicy& ssaaPolicy();

// The inactive policy: every function present, every answer no. Returned when
// the engine is unknown, and used by the Ayesha policy for the two clamp
// entries it has no use for.
const SsaaPolicy& ssaaNoPolicy();

// The size of the colour target currently bound to `context`, read from the
// target itself rather than from anything cached.
//
// Lives in core because reading a bound render target is not an engine
// question, but WHICH source is authoritative is -- so only Ayesha's policy
// calls this, and KTGL's deliberately does not.
bool ssaaBoundColorTargetSize(ID3D11DeviceContext* context,
                              unsigned int* width, unsigned int* height);

}  // namespace atfix
