// SPDX-License-Identifier: MIT
//
// Pace the render loop while the game's window is hidden.
//
// THE DEFECT. Escha & Logy and Shallie present as fast as the hardware allows
// once their window is minimized. Nothing in either game limits the frame rate;
// vblank is the only thing pacing them, and a minimized swap chain stops
// waiting for it. Measured on Escha under Proton, both display modes, with the
// window hidden for six to fifteen seconds each time:
//
//   fullscreen   141 fps visible ->  2670 fps hidden   (19x)
//   windowed     172 fps visible ->   620 fps hidden   (3.6x)
//
// Present returns DXGI_STATUS_OCCLUDED on every hidden frame in both modes, and
// IsIconic agrees with it on every sample.
//
// THE CORRECTION IS THE ENGINE'S OWN. PhyreEngine already does this, once per
// frame, and the KTGL rewrite dropped it. The idiom is identical in all four
// PhyreEngine builds -- fetch the window from the application object, call
// IsIconic, and Sleep(0x21) if it is minimized:
//
//   Atelier_Ayesha_EN.exe     0x19e9c0, IsIconic at 0x19e9ea
//   Atelier_Ayesha.exe        0x1a3d20, IsIconic at 0x1a3d4a
//   A11R_x64_Release_en.exe   0x12e260, IsIconic at 0x12e28a
//   A12V_x64_Release_en.exe   0x18c4b0, IsIconic at 0x18c4da
//   A13V_x64_Release_EN.exe   0x154ab0, IsIconic at 0x154ada
//
// Neither KTGL executable imports IsIconic at all: 368 imports in Escha & Logy
// and 359 in Shallie, no entry in either, while Ayesha has one. That is the
// whole defect. So the correction restores the sibling engine's behaviour at
// the sibling engine's own number, 33 ms, rather than inventing a rate.
//
// 33 ms is a THROTTLE, NOT A STOP. The game still renders about 30 frames a
// second while hidden, which is exactly what the four PhyreEngine games do
// today. Anything driven from the frame loop keeps running, only slower.
//
// WHY BOTH SIGNALS. The HRESULT alone was enough in every measurement, but
// every one of those ran through DXVK, and DXVK's occlusion reporting is its
// own implementation rather than native DXGI's. IsIconic is a Win32 call that
// behaves the same on both, so reading both costs one cached handle and removes
// the need to have measured Windows separately.
//
// WHY CORE AND NOT engines/ktgl. This touches no game code, needs no address
// pack and no build fingerprint. It is DXGI and user32 only. The capability
// matrix is how core says Ayesha does not need it, without naming an engine.
//
// AYESHA IS Unsupported BECAUSE THE DEFECT IS NOT THERE, not for want of a
// mechanism. Stacking this on top of the engine's own throttle would halve its
// hidden frame rate for no reason. The same argument keeps it out of the Arland
// project entirely: all three of those games carry the idiom already.
//
// DUSK_MINIMIZED_THROTTLE overrides the millisecond count for a diagnostic run.
// 0 disables the correction; anything above 1000 is ignored as a typo.
#pragma once

struct IDXGISwapChain;

namespace atfix {

// Called from the Present hook with the result of the real Present, AFTER it
// returns. Sleeps when the frame went nowhere. A visible frame costs one
// integer compare and one cached IsIconic call.
void presentThrottleAfterPresent(IDXGISwapChain* swapChain, long result);

}  // namespace atfix
