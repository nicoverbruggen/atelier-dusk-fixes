// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace atfix {

// Whether SMAA post-processing is enabled. Supported and on by default in all
// three games; see the capability matrix in game.cpp.
bool smaaEnabled();

// Whether the pre-UI path is wanted. On by default when SMAA is on, because it
// is strictly better where it works: the Present path softens the interface,
// which is the reason SMAA had to be switched off after its first validation.
// DUSK_SMAA_PREUI=0 falls back to the Present path for comparison.
bool smaaPreUiEnabled();

// Run the SMAA passes over the swap chain's back buffer, in place, just before
// Present. No-op unless enabled and the resources initialize.
//
// This is the full-frame path: it antialiases the composited frame, UI and all,
// because it runs after everything has been drawn. That is a real cost -- text
// and HUD edges are softened along with the scene -- and it is why this is the
// first half of the feature rather than the whole of it. The pre-UI injection
// that avoids it needs a scene/UI boundary that has not been established for
// this engine yet.
//
// Best-effort: any failure disables SMAA for the rest of the session and leaves
// the rest of the mod alone.
void smaaApply(IDXGISwapChain* swapChain);

// Run the passes over the scene colour target, in place, at whatever size that
// target is -- which under supersampling is larger than the display.
//
// THIS IS THE PRE-UI PATH, and on this engine it needs none of the guesswork
// the Arland implementation does. There, everything composites into one
// surface, so the scene/UI boundary has to be inferred from a three-condition
// depth-state heuristic. Ayesha composites its interface onto the back buffer
// separately, so the scene target simply stops being the render target -- that
// transition IS the boundary, and it is observed rather than guessed.
//
// Two things fall out of running here rather than at Present. The interface is
// not yet drawn, so text and 2D art are left alone -- the reason the Present
// path had to be turned off. And under supersampling the passes run at the
// enlarged size and are then box-downscaled with the rest of the scene, which
// is where a morphological filter is most effective: it reconstructs the
// near-vertical and near-horizontal edges an ordered supersampling grid handles
// worst.
//
// Saves and restores every piece of pipeline state it touches, scissor rects
// included. Returns true if it ran, and runs at most once per frame. The pre-UI
// and Present routes share one guarded resource tuple tied to the first game
// device measured in the process; an overlapping call or second device is
// refused rather than receiving another device's resources.
bool smaaApplySceneColor(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene);

// Clears the once-per-frame latch. Called at Present.
void smaaFrameReset();

}  // namespace atfix
