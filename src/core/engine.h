// SPDX-License-Identifier: MIT
#pragma once

// The engine dispatch layer, and the reason this project is one DLL rather than
// three.
//
// The Dusk trilogy spans two engines (`core/game.h`, Engine), and a fix written
// against one is meaningless against the other: Ayesha's font-atlas path has no
// homolog in Escha & Logy or Shallie, and their open defects -- Escha's shadow
// SRV, Shallie's CreateSamplerState -- have none in Ayesha. So the fixes are
// split into `src/engines/phyre/` and `src/engines/ktgl/`, which share nothing but `src/core/`.
//
// They still ship as a single d3d11.dll. Address-based and shared Direct3D fixes
// are gated on both the capability matrix in game.cpp and an exact executable
// fingerprint, so a module in the wrong process installs neither. The narrow
// early-window exception is described below. Splitting the artifact would buy
// no further safety and cost the user a per-game download.

namespace dusk {

// The fixes that must be installed from DllMain, before the game reaches D3D11.
// A window is created earlier than that, so a hook placed when the proxy is
// first used is already too late for anything window-shaped.
//
// Separate from initializeEngineFixes() because it runs under the loader lock,
// where almost nothing is safe. Exact .text recognition is not available this
// early, so these hooks are the deliberate exception: they may only mutate a
// call that identifies the game window through narrow runtime facts (module,
// class/brush or measured size), and must forward every other call unchanged.
// Both modules may leave this empty.
void installEngineEarlyFixes();

// Resolves the engine for this process and initializes that module's fixes.
// Idempotent, and safe to call from every D3D11 entry point -- which it is,
// because the point at which the game reaches D3D11 is the earliest we can be
// sure its image is fully unpacked. Returns true only for an exact executable
// fingerprint; core uses that answer to gate its shared D3D mutations too.
bool initializeEngineFixes();

// Called from the hooked Present. Frame boundaries mean different things to the
// two modules (for Phyre it is the atlas cache's whole lifetime), so this only
// forwards to whichever one initialized.
void engineFrameTick();

}  // namespace dusk
