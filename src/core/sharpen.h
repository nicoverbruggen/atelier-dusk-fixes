// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Contrast-adaptive sharpening, run over a surface in place.
//
// WHY IT PAIRS WITH SMAA. SMAA is morphological: it finds an edge and blends
// across it, which removes the staircase and softens the edge in the same
// stroke. A sharpening pass afterwards makes it look crisp again without
// bringing the staircase back, because it works on local contrast rather than
// on geometry. Supersampling in this mod already folds a sharpen into its downscale
// for exactly this reason -- a box filter is an average and an average is a
// blur -- so the pre-UI antialiasing path having none was the odd one out.
//
// THE FILTER IS AMD'S CAS, not an unsharp mask. The difference matters at the
// extremes: an unsharp mask sharpens uniformly and rings on already-hard edges,
// while CAS derives its weight from the local minimum and maximum so flat
// regions are left alone and high-contrast edges are sharpened LESS than
// mid-contrast ones. On a 2013 game being magnified that is the behaviour worth
// having: it lifts texture detail without haloing the interface art or the hard
// edges of the world.
//
// RUN AFTER THE ANTIALIASING, never before. Sharpening first would give SMAA
// harder edges to find and it would blend them away again, which is work spent
// to arrive back where it started.
//
// COLOUR ONLY. KTGL consumes the interface target's existing alpha in later
// composition. Writing the shader's constant alpha=1 into that channel caused
// intermittent black terrain polygons in Shallie; an RGB-only blend write mask
// fixed the defect in both sharpen-only and SMAA-plus-sharpen runs. The pass
// must preserve alpha even if a future shader returns another value there.
namespace atfix {

// How hard to sharpen, 0 to 1. `[Rendering] Sharpen` or DUSK_SHARPEN as a
// percentage; 0 disables the pass entirely.
float sharpenAmount();

// Shorthand for sharpenAmount() > 0, for the gates that only ask whether the
// pass is on at all. It is INDEPENDENT of edge smoothing: the two run at the
// same moment and one is not a prerequisite for the other. Sharpening a frame
// that was never antialiased is a legitimate thing to want -- it is what the
// setting means when edge smoothing is off.
bool sharpenEnabled();

// Load the shader compiler from the frame tick rather than from a draw. See
// sharpen.cpp: doing it inside a draw detour deadlocked on the loader lock.
void sharpenPreload();

// Sharpen `target` in place. Copies it to a scratch first, because a shader
// cannot read and write one resource in the same draw -- the runtime unbinds
// one of them and the result is silently wrong.
//
// Returns false when the pass is off, could not be built, the surface is a
// shape it does not handle, or a different D3D device owns the shared resource
// tuple. The tuple is mutex-protected; a refusal costs sharpness and nothing
// else.
bool sharpenApply(ID3D11DeviceContext* ctx, ID3D11Texture2D* target);

}  // namespace atfix
