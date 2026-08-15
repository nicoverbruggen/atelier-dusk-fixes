// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

// Black bars on a display that is not 16:9, so the picture keeps its shape.
//
// THE DEFECT. KTGL lays its interface out in a fixed 1920x1080 canvas whose
// aspect no code computes from the back buffer, so a 4:3 frame does not make it
// render 4:3 -- it renders its 16:9 picture stretched across the width.
//
// THIS FILE OWNS THE SHAPE QUESTION AND NOTHING ELSE: is this back buffer 16:9,
// and what is the centred 16:9 rectangle inside it. How a frame reaches the
// back buffer is an engine fact -- one draw on PhyreEngine, four a frame on
// KTGL -- so the pass that acts on the answer is registered by the engine
// module. See engines/ktgl/letterbox_present.h.
//
// AYESHA IS CORRECTED IN THE LAUNCHER, which is why its matrix row is
// Unsupported rather than the game being fine. PhyreEngine fits a 16:9 render
// into the back buffer itself and stretches any other render shape, so writing
// it a 16:9 render size is the whole correction (launcher/aspect_fit.h); a pass
// here as well is a second fit and the picture comes out too small.
// DUSK_LETTERBOX=0 tells that apart from no fit at all, because too small and
// too wide look alike.
//
// The one-percent tolerance: 1366x768 is off 16:9 by 0.05%, and correcting it
// would buy a two-pixel bar down each side.
namespace atfix {

// Called from both swap-chain creation hooks. Reads the chain's back buffer,
// answers the shape question once, and tags the texture so a later bind can be
// recognised as the composite. Tagged here rather than borrowed from
// supersampling, whose tag is only applied when that feature is active and the
// bars are needed either way.
void letterboxNoteSwapChain(IDXGISwapChain* swapChain);

// The pass that actually fits the frame, supplied by whichever engine needs
// one. Core owns the SHAPE question -- is this back buffer 16:9, and what is
// the centred rectangle inside it -- and nothing else. How a frame reaches the
// back buffer is an engine fact: one draw on PhyreEngine, four on KTGL. See
// engines/ktgl/letterbox_present.h.
using LetterboxFitPass = bool (*)(IDXGISwapChain*);
void letterboxSetFitPass(LetterboxFitPass pass, void (*preload)());

// Load whatever the registered pass needs, from the frame tick rather than from
// a draw. LoadLibrary inside a draw detour takes the loader lock on whichever
// thread the engine is recording on, which has hung this engine before.
void letterboxPreload();

// Fit the frame. Called last at Present, after every other pass has finished
// writing it, because no earlier point knows the frame is finished. Declines
// when the feature is off for this game, when the back buffer is already 16:9,
// and when no engine registered a pass.
bool letterboxApply(IDXGISwapChain* swapChain);

// Whether this session has a back buffer whose shape needs correcting. False
// for 16:9, for anything within one percent of it, and before the back buffer
// has been seen.
bool letterboxActive();

// The centred 16:9 rectangle inside a target of this size, or false when this
// game does not want a fit, when the shape needs no correction, or when the
// size is not the back buffer's. This is the arithmetic and the gate together,
// so a pass can ask one question and act on the answer.
bool letterboxViewportFor(UINT targetWidth, UINT targetHeight,
                          D3D11_VIEWPORT* viewport);

}  // namespace atfix
