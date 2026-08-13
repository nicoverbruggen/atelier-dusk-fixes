// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

// Supersampling: render the scene larger than it will be displayed, so every
// displayed pixel is the average of several rendered ones. The bluntest
// antialiasing there is, and the only one that improves everything at once --
// geometry edges, texture interiors, alpha-test edges, shader aliasing --
// because it raises the sampling rate of the whole image rather than treating
// one class of edge.
//
// FOUR IMPLEMENTATIONS PRECEDED THIS ONE AND NONE OF THEM WORKED. The list is
// here rather than in a document because three of the four failed on a detail
// this header is the natural place to warn about, and the fourth failed on a
// detail the ordering notes below exist to prevent:
//
//   1. BACK-BUFFER REDIRECT (the Arland design, ported). Substituted a larger
//      texture when the game made a render-target view over the swap chain's
//      back buffer, and averaged it down at Present. Black screen: Ayesha
//      created exactly one such view, at startup, and never composited through
//      it. The scene is never in the back buffer on this engine.
//   2. ENLARGE THE SCENE TARGETS AND LET THE ENGINE RESAMPLE. Worked, but the
//      engine's composite samples through a bilinear sampler -- four taps,
//      which only resamples correctly at a whole-number ratio. It also silently
//      broke the scene test, because "how big are the scene targets" was
//      written down in both the resize and the test and the two disagreed the
//      moment this was switched on.
//   3. OWN THE DOWNSCALE. A ratio-sized box filter substituted at the
//      composite's sample, later with a sharpen folded in. "Better but not
//      much" -- it ran on every scene-to-non-scene transition, of which there
//      are 5 to 22 per frame.
//   4. ADD A ONCE-PER-FRAME LATCH TO (3). Black 3D scene, missing interface:
//      the first transition each frame is a post-processing bind, not the
//      composite, so the composite was handed a copy of a half-finished scene.
//
// THE LESSON, and the whole design of what follows: "the engine stopped
// rendering into the scene target" is not "the composite is about to run". It
// fires many times a frame while the post-processing chain steps in and out of
// the two ping-ponged scene colour targets. The composite has to be identified
// POSITIVELY.
//
// SO THIS IDENTIFIES IT BY THE SWAP-CHAIN BACK BUFFER. The mechanism, end to
// end:
//
//   - highres.cpp already rewrites this engine's pinned 1920x1080 scene targets
//     to the main render size, and carries the hard-coded viewport and scissor
//     with them (validated in game at 1440p and 4K). Supersampling multiplies
//     that one size -- see highResSceneSize, which is its sole owner. The
//     engine then renders and post-processes at the enlarged size, untouched.
//   - The back buffer is tagged at swap-chain creation (ssaaNoteBackBuffer).
//   - A bind whose colour render target carries that tag is the composite, and
//     nothing else in this renderer is (ssaaNoteTargetsBound). That is a
//     runtime fact, not a heuristic, and it is exact.
//   - While that marker is set, the moment the engine binds a scene colour host
//     as a pixel-shader resource is the composite's sample -- and that sample
//     IS the resample. ssaaSubstituteShaderResources box-filters the host down
//     to display size and substitutes a view over the result IN THE FORWARDED
//     ARGUMENT ARRAY OF THAT ONE CALL. Nothing is latched across the call, so
//     no later pass can be poisoned by a substitution that stayed armed.
//   - Nothing happens at Present except counters. That is the safety argument:
//     the failure mode of attempts 1 and 4 -- a pass painting over a frame the
//     game had already drawn correctly -- is structurally unreachable, because
//     this module never draws over anything the player sees.
//
// Substituting at the SAMPLE rather than at the BIND is also what resolves the
// ping-pong: there are two byte-identical scene colour targets, and only the
// view the composite actually binds says which one it reads.
//
// IF IDENTIFICATION NEVER FIRES, the engine's own bilinear downscale of the
// enlarged scene is what the player sees -- attempt 2's picture, soft but
// correct -- and ssaaFrameTick names that state in the log rather than leaving
// it to be inferred from a counter that stays at zero.
//
// This requires the high-resolution fix, which is on by default above 1080p,
// and does nothing without it.
namespace atfix {

// ---- configuration --------------------------------------------------------

// The supersampling factor as a percentage: 100 = off, 150 = render the scene
// at one and a half times the width and height. A percentage rather than a
// decimal because "1.5" in an ini is a locale trap -- a comma-decimal locale
// parses it as 1. Never returns below 100.
unsigned int ssaaPercent();

// Shorthand for ssaaPercent() > 100, for the gates that only ask whether the
// feature is on at all.
bool ssaaConfigured();

// Whether supersampling has actually substituted at least once this session --
// "configured" and "engaged" are different questions and callers that stand
// something else down must ask the second one. See scene_pass.cpp's boundary.
bool ssaaEngaged();

// How hard to sharpen after the downscale, 0 to 1. A box filter is an average
// and an average is a blur, so a correct downscale is softer than its source --
// which is why resampling is normally paired with a sharpen (FSR does exactly
// this with RCAS). Folded into the downscale shader rather than run as its own
// pass. `[Rendering] SupersamplingSharpen` or DUSK_SSAA_SHARPEN, as a
// percentage; 0 disables it.
float ssaaSharpen();

// The scene render size for a given main render size, with the factor applied
// and clamped so an over-ambitious setting cannot ask for a target no driver
// will allocate. Returns false and leaves the outputs alone when supersampling
// is off, so a caller can use it unconditionally.
//
// NOT A PUBLIC ANSWER TO "how big are the scene targets" -- highResSceneSize is
// that, and it is the only thing entitled to answer it. This is the arithmetic
// that function calls. The one time those two facts had separate definitions
// they drifted apart and stopped the scene test matching for a whole session.
bool ssaaSceneSize(unsigned int mainWidth, unsigned int mainHeight,
                   unsigned int* sceneWidth, unsigned int* sceneHeight);

// ---- identification -------------------------------------------------------

// Tag the swap chain's back buffer, from both swap-chain creation paths.
//
// Identity by TAG rather than by a held reference or a raw pointer, and both
// alternatives are wrong in ways that would not show up until later: holding a
// reference would block ResizeBuffers, and remembering the address would match
// whatever the allocator later hands back at that address. The tag lives on the
// resource and dies with it. ssaaFrameTick re-verifies it cheaply once a frame,
// so a back buffer recreated behind our back is re-tagged rather than lost.
void ssaaNoteBackBuffer(IDXGISwapChain* swapChain);

// Tag a colour target that the engine's scene test has just identified as a
// scene colour host. Called from scenePassNoteBoundary, which owns that verdict
// -- this module stores it and nothing more, so there is still exactly one
// place that decides which surface is the scene.
//
// No-op when supersampling is off.
void ssaaTagSceneHost(ID3D11Texture2D* sceneColor);

// Does this surface carry the tag above, and is it the back buffer? Both false
// when supersampling is off, because nothing is tagged then.
//
// For an anchor that has to pick one surface out of several that look alike.
// The tags are positive identification -- one comes from the engine's own scene
// test, the other from the swap chain itself -- where size and format are only
// a resemblance, and with supersampling on there is more than one full-screen
// typeless colour target for a size rule to match.
bool ssaaIsSceneHost(ID3D11Texture2D* texture);
bool ssaaIsBackBuffer(ID3D11Texture2D* texture);

// Set or clear this context's composite marker from the render targets now
// arriving. The marker is set exactly while a colour target carrying the back
// buffer's tag is bound.
//
// MUST RUN AFTER scenePassNoteBoundary, which is what tags the arriving surface
// as a scene colour host. See the ordering note in scene_pass.cpp's
// hookedOMSetRenderTargets.
void ssaaNoteTargetsBound(ID3D11DeviceContext* context, unsigned int numViews,
                          ID3D11RenderTargetView* const* views);

// The composite's sample. If this context's composite marker is set and one of
// these views reads a tagged scene colour host, box-filter that host down to
// display size (running SMAA on the result if it is enabled) and write a copy
// of the argument array into `substituted` with that one slot replaced.
//
// How large the caller's stack array must be. A composite that sets more
// resources at once than this is refused rather than guessed at, and says so
// once in the log; the engine's own resample then stands. Sixteen is the
// classic shader-model resource-slot count and comfortably above the ten SMAA
// itself binds -- the widest single call this project has ever seen here.
constexpr unsigned int kSsaaMaxSubstitutedViews = 16;

// Returns true when `substituted` should be forwarded instead of `views`. The
// occupant check, downscale and shared tuple publication are one nonblocking
// transaction tied to the first measured game device. The replacement view is
// retained across that guard; after the forwarded PSSetShaderResources call
// has taken its own reference, the caller releases every entry that differs
// from `views`.
//
bool ssaaSubstituteShaderResources(ID3D11DeviceContext* context,
                                   unsigned int startSlot,
                                   unsigned int numViews,
                                   ID3D11ShaderResourceView* const* views,
                                   ID3D11ShaderResourceView** substituted,
                                   unsigned int capacity);

// Put the composite's viewport back to the presented size.
//
// The substitution above fixes what the composite READS. This fixes where it
// DRAWS: on the clamp route the engine still believes it is rendering at the
// multiplied size, so its viewport covers more than the clamped back buffer and
// the frame comes out cropped rather than scaled. Called from the draw detours,
// because only at the draw are the viewport and the render target both current.
//
// No-op on Ayesha, where the raster correction in highres.cpp owns this.
void ssaaCorrectCompositeViewport(ID3D11DeviceContext* context);

// Drop this context's composite marker when D3D resets that context to default
// state. `FinishCommandList(FALSE)` does; `FinishCommandList(TRUE)` restores
// the state it had before the call and therefore deliberately keeps the marker.
void ssaaClearContextState(ID3D11DeviceContext* context);

// ---- the two engine routes ------------------------------------------------
//
// Supersampling is one feature reached by two opposite routes, and everything
// above is the Ayesha one: the mod enlarges the engine's pinned scene targets
// and substitutes its own downscale at the composite's sample.
//
// KTGL takes the other route -- it sizes everything from its own ini, so the mod
// clamps the swap chain back down and lets the engine's device init take its own
// offscreen branch. That code lives in engines/ktgl/present_clamp.cpp, and which
// route this process takes is answered by supersample_policy.h.

// Is either supersampling route on? The Ayesha route is `ssaaConfigured()`, the
// KTGL one is the clamp above. Everything that only needs to know "is this
// feature doing anything this session" asks this rather than either half, so a
// gate cannot be updated for one route and silently skipped for the other.
bool ssaaActive();

// Clamp a requested back-buffer size down to the display size. No-op unless the
// clamp is enabled and the request is genuinely larger. `where` names the call
// site for the log, which reports each distinct clamp once.
void ssaaClampPresentSize(UINT* width, UINT* height, const char* where);

// Resize a windowed swap chain's output window so its client area matches the
// clamped back buffer.
//
// Only the KTGL route needs this, and only windowed. Supersampling there works
// by writing the game's own ini at base x factor so the engine renders
// everything larger, then clamping the swap chain back down. The engine also
// sizes its WINDOW from that ini, so at 150% the player gets a 2880x1620 window
// containing a 1920x1080 image. Fullscreen hides it because the display mode
// decides the size instead; windowed does not.
//
// Does nothing when the description is fullscreen, when the clamp route is off,
// or when the window already has the right client area.
void ssaaFitOutputWindow(const DXGI_SWAP_CHAIN_DESC* desc);

// Called at Present. Counters, the periodic line, and the one-shot lines that
// name the states in which this feature is configured and doing nothing.
//
// NO RENDERING WORK HAPPENS HERE, deliberately and permanently. Two of the four
// failed attempts blacked the screen out with a present-time pass.
void ssaaFrameTick(IDXGISwapChain* swapChain);

}  // namespace atfix
