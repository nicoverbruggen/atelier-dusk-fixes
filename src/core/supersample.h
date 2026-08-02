// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Supersampling: render the scene larger than it will be displayed, so every
// displayed pixel is the average of several rendered ones. The bluntest
// antialiasing there is, and the only one that improves everything at once --
// geometry edges, texture interiors, alpha-test edges, shader aliasing --
// because it raises the sampling rate of the whole image rather than treating
// one class of edge.
//
// THE FIRST ATTEMPT WAS ATTACHED IN THE WRONG PLACE, and this one is not a
// refinement of it but a different mechanism.
//
// That version was the Arland implementation ported directly: it substituted a
// larger texture whenever the game created a render-target view over the swap
// chain's back buffer, then averaged that texture down at Present. It is the
// right design for the Arland games, which composite into the back buffer.
// Ayesha created exactly one such view, at startup, and none afterwards. The
// substitute stayed empty and the downscale painted it over a frame the game
// had already drawn correctly: a black screen, with a `redirects=1` counter as
// the only evidence.
//
// The MSAA work later explained why, and the explanation is what this
// implementation is built on. Ayesha renders its scene into offscreen typeless
// colour targets and only composites to the back buffer at the end -- measured,
// not assumed (WORK_DOC.md, "MSAA and supersampling", the three twinned pair
// descriptors). There is nothing to redirect at the back buffer because the
// scene was never there.
//
// SO THIS SCALES THE SCENE TARGETS INSTEAD, and needs almost no machinery of
// its own, because the high-resolution fix already built all of it:
//
//   - `highres.cpp` already rewrites the engine's hard-coded 1920x1080 scene
//     targets to the main render size. Multiplying that size is the entire
//     change.
//   - Its raster correction already rewrites a submitted viewport and scissor
//     to the size of whatever target is actually bound, so the scene pass
//     follows the enlarged target without knowing anything happened.
//   - The engine's own composite pass samples the scene target into the back
//     buffer through an ordinary sampler, which IS the downscale. No extra
//     pass, no extra shader, and nothing at Present.
//
// It therefore requires the high-resolution fix to be on, which it is by
// default above 1080p, and it does nothing without it.
//
// WHAT REMAINS UNMEASURED, and it is the thing most likely to be wrong: whether
// this engine's post-processing derives its texel offsets from the target it is
// sampling or from the screen size it expects. A shader that hard-codes the
// latter would sample at the wrong stride once the scene target is larger --
// visible as blur, ghosting or misplaced bloom rather than as a clean failure.
// One run answers it, and that is why this ships opt-in.
namespace atfix {

// The supersampling factor as a percentage: 100 = off, 150 = render the scene
// at one and a half times the width and height. A percentage rather than a
// decimal because "1.5" in an ini is a locale trap -- a comma-decimal locale
// parses it as 1. Never returns below 100.
unsigned int ssaaPercent();

// The scene render size for a given main render size, with the factor applied
// and clamped so an over-ambitious setting cannot ask for a target no driver
// will allocate. Returns false and leaves the outputs alone when supersampling
// is off, so a caller can use it unconditionally.
bool ssaaSceneSize(unsigned int mainWidth, unsigned int mainHeight,
                   unsigned int* sceneWidth, unsigned int* sceneHeight);

}  // namespace atfix
