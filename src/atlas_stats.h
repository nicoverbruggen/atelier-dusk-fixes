// SPDX-License-Identifier: MIT
#pragma once

namespace dusk {

// Installs the Ayesha font-atlas diagnostic hooks if this process is a
// recognized Ayesha build and DUSK_ATLAS_STATS is set. Idempotent; safe to call
// from every D3D11 entry point. Returns true if the hooks are installed and
// counting.
bool initializeAtlasStats();

// Called from Present. Closes out the frame's counters so out-of-drain locks can
// be attributed to a frame, which is what distinguishes the Rorona-class
// (frame-scoped) case from the Totori/Meruru-class (queue-scoped) one.
void atlasStatsFrameTick();

}  // namespace dusk
