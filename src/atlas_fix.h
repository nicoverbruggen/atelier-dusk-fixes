// SPDX-License-Identifier: MIT
#pragma once

namespace dusk {

// Installs the Ayesha font-atlas hooks if this process is a recognized Ayesha
// build and at least one atlas feature is enabled: DUSK_ATLAS_CACHE (the fix) or
// DUSK_ATLAS_STATS (the diagnostic). Idempotent; safe to call from every D3D11
// entry point. Returns true if the hooks are installed.
bool initializeAtlasFix();

// Called from Present. This is the atlas cache's frame boundary: Ayesha measured
// as the frame-scoped (Rorona-class) case, so every snapshot is discarded here.
// It also closes out the diagnostic's per-frame counters.
void atlasFixFrameTick();

}  // namespace dusk
