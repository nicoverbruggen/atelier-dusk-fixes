// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

// Startup logo skip for Ayesha.
//
// Ported from the Arland project's src/engines/phyre/logo_skip.h, which is this project's
// own code (MIT). The mechanism is identical because the code is: Ayesha runs
// the same PhyreEngine boot sequence, and both hooked functions are
// byte-identical to the Arland ones over their prologue windows.
//
// The boot logos are not part of the title-screen state machine. They belong to
// ThreadEasyRenderLogo, a small object the application creates before it starts
// initialising the engine. Its update runs on the render thread and steps a
// phase sequence over fullscreen picture layers. The application does not wait
// for the logos before loading: it creates the object, performs the whole engine
// and resource initialisation while the render thread animates, and only then
// spins until the sequence reports its terminal phase. A separate title-side
// player blocks on the same object for the attract replay after an idle title
// screen. Both wait on nothing but the phase field, so writing the terminal
// phase releases both.
//
// TWO CONSEQUENCES A READER SHOULD EXPECT, both confirmed in Ayesha's own boot
// function rather than carried over from Arland:
//
//   Skipping does not start the game sooner. The logos play while the game
//   loads, so this shows the clear colour for as long as loading genuinely
//   takes. That is the honest presentation and it is not a defect.
//
//   The idle-title attract replay stops happening, because the second waiter
//   polls the same field.
//
// Two hooks, because one is not enough to guarantee a clean screen. Forcing the
// phase stops the sequence advancing, but the picture layers are already
// constructed and their alpha only reaches the material when the layer's own
// update runs, which the forced path no longer calls. Rather than reason about
// what a never-ticked layer draws, the draw is suppressed as well.
//
// The object is left structurally intact, so the game's own destructor still
// frees the picture layers.
namespace atfix {

// Skip the boot logos. Installs only when the capability matrix supports the
// feature and the user opted in. `exeBuild` is the verified build the Phyre
// module already resolved. Returns true when both hooks are live.
bool installLogoSkip(BYTE* base, uint8_t exeBuild);

}  // namespace atfix
