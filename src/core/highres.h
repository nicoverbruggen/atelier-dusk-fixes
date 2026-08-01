// SPDX-License-Identifier: MIT
#pragma once

// High-resolution rendering, and the census that measured the need for it.
//
// The two live in one file because they are one subsystem: both hang off the
// same `ID3D11Device::CreateTexture2D` hook, and MinHook allows exactly one
// hook per target, so they could not be separate installs even if the split
// were desirable. It is not -- the census is how the fix is verified, and
// running them together is what makes "did this actually resize everything"
// answerable in a single session.
//
// THE DEFECT, measured rather than assumed (WORK_DOC.md, "Measured: Ayesha has
// the old-Arland defect"). Ayesha takes any resolution from its own
// Setting.ini and creates a swap chain and matching depth target at that size,
// but every target the scene is actually drawn into is created at a hard-coded
// 1920x1080. The scene is therefore rendered at 1080p and scaled up, and
// choosing 1440p buys a larger window and no more detail.
//
// THE FIX is the one TellowKrinkle established for this engine family and the
// Arland project refined: learn the main render size from the first
// depth-stencil target the game creates, then give every later hard-coded
// target the same size, and correct the hard-coded viewport and scissor to
// match. It is a pure D3D11-layer correction -- no game addresses, no
// prologues, nothing engine-specific -- which is why it lives in core rather
// than in src/phyre despite currently applying only to Ayesha.
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace atfix {

// Installs the device and context hooks. Idempotent, and safe to call from
// every device-creation path. Installs nothing when neither the fix nor the
// census is enabled, so an ordinary run has no hooks here at all.
void initializeHighRes(ID3D11Device* device, ID3D11DeviceContext* context);

// Called from the hooked Present. Emits the census summary, which carries the
// number of creations seen: a census whose only output is "nothing found" is
// indistinguishable from one that never installed.
void highResFrameTick();

// Records the size the swap chain was actually created at. Logged once, and
// independently of whether either feature is enabled -- one line naming the
// present resolution is worth having in every log.
void noteSwapChainSize(unsigned int width, unsigned int height,
                       unsigned int format, unsigned int refreshNumerator,
                       unsigned int refreshDenominator, bool windowed);

}  // namespace atfix
