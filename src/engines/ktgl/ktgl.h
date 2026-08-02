// SPDX-License-Identifier: MIT
#pragma once
//
// The LTGL/KTGL module: Atelier Escha & Logy DX and Atelier Shallie DX.
//
// These two are UCRT builds on the newer Gust engine. They carry NO known menu
// hitch -- the Arland/Ayesha text renderer has no homolog in either binary and
// their queue drain mismatches on both vote and prologue shape (WORK_DOC.md
// 1.3) -- so nothing in `src/engines/phyre/` applies to them and none of it is reachable
// here. Their own open defects are different problems entirely:
//
//   * Escha & Logy: the shadow-texture bug AGT worked around by replacing the
//     SRV at init, and the system-save-data wipe on quit.
//   * Shallie: the CreateSamplerState bug AGT patched.
//
// None is implemented yet, so this module currently only fingerprints. The four
// executable identities are verified; this check is the gate every future fix
// here will install behind.

namespace dusk {

// Fingerprints the process against the known Escha & Logy / Shallie builds and
// records the result. Returns true if the executable was recognized. Installs no
// hooks -- there are none yet.
bool initializeKtglFixes();

// Present. Nothing here needs a frame boundary yet; the hook exists so that the
// first fix which does can use it without touching the dispatch layer.
void ktglFrameTick();

}  // namespace dusk
