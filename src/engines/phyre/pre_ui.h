// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Ayesha's pre-UI moment.
//
// WHY IT IS NOT THE SCENE-TARGET TRANSITION, which is where it used to be. This
// engine ping-pongs between two identically-shaped scene colour targets through
// its post-processing chain -- `scene_target.cpp` says so, and says both of them
// are the scene, which is correct for the question that file answers. It is the
// wrong answer for this one. The transition fires when the engine leaves the
// FIRST of the pair, which is somewhere in the middle of the post chain, and
// because the pass claims the frame's one SMAA run it then also stopped the
// real moment from ever being reached.
//
// The symptom was a working log and an unchanged picture: "SMAA: pre-UI active"
// every session, no visible smoothing in game, and -- the observation that
// settled it -- DUSK_SMAA_DEBUG=1 drawing its edge map on the title screen,
// where the Present path runs, and nothing at all in gameplay, where this path
// runs. An edge map written into a surface that reached the screen could not
// have been missed.
//
// WHY THE TRANSITION CANNOT BE IT, recorded in scene_pass.cpp before this file
// existed and removed from there with the dead branch: the scene-to-not-scene
// transition happens 5-22 times per frame, because the engine steps in and out
// of its scene targets while running its post-processing chain. Only the last
// one is the composite, and SMAA's once-per-frame latch makes the FIRST one
// win.
//
// THE SCENE TARGET IS THE ONE THE 3D PASS IS DRAWN INTO, and it is recognised
// by counting those draws rather than by its shape. This is Arland's SceneRt
// boundary, which has run in three shipped games; sync_fix.cpp states it as
// "only the 3D pass accumulates hundreds of them", with a threshold of 24.
//
// Two anchors were tried here first and both were resemblances. The
// size-and-format rule matches both halves of the ping-pong pair, so it fired
// mid-chain. "The first draw into the back buffer" fired on whatever the engine
// draws there before the composite: the log line printed and the picture was
// unchanged, on a run on 2026-08-08.
namespace atfix {

// Called from the render-target bind, before it is forwarded. Fires the passes
// on the surface being left when that surface received the frame's 3D pass.
void phyrePreUiNoteTargets(ID3D11DeviceContext* context, unsigned int numViews,
                           ID3D11RenderTargetView* const* views);

// Called from whichever draw detour is installed. Counts the draw against the
// currently bound target, which is all the identification the anchor needs.
void phyrePreUiAfterDraw(ID3D11DeviceContext* context);

// Called at Present. Drops the tracked target, so the next frame's first
// surface is not credited with this frame's draws.
void phyrePreUiFrameTick();

}  // namespace atfix
