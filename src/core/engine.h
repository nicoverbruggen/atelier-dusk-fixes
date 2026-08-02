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
// They still ship as a single d3d11.dll. Every fix is already gated twice, on
// the capability matrix in game.cpp and on an executable fingerprint, so a
// module in the wrong process installs nothing; splitting the artifact as well
// would buy no safety and cost the user a per-game download. What the engine
// axis buys is that neither module has to know the other exists.

namespace dusk {

// Resolves the engine for this process and initializes that module's fixes.
// Idempotent, and safe to call from every D3D11 entry point -- which it is,
// because the point at which the game reaches D3D11 is the earliest we can be
// sure its image is fully unpacked. Returns true once a module has taken over.
bool initializeEngineFixes();

// Called from the hooked Present. Frame boundaries mean different things to the
// two modules (for Phyre it is the atlas cache's whole lifetime), so this only
// forwards to whichever one initialized.
void engineFrameTick();

}  // namespace dusk
