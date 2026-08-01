// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

#include "../core/hook_util.h"

namespace atfix {

// Experimental Ayesha field-jitter investigation. The implementation is a
// static port of Arland's threshold rescale and resting stabilizer, but an
// Ayesha runtime test established that it does not fix the problem. The anchor
// addresses are verified; the Ayesha-specific cause and live controller layout
// are not. Both writes therefore remain opt-in investigation switches.
//
// DUSK_FIELD_TRACE=1 logs the carried-over controller fields around each
// ground-contact change. Use it for fresh Ayesha-specific analysis; plausible
// output alone does not validate the current fix.
//
// DUSK_FIELD_ENGINE_FIX requests the threshold rescale.
// DUSK_FIELD_STABILIZER requests the controller-object writes and requires the
// rescale. See WORK_DOC.md, "Open field-jitter investigation".
// `exeBuild` selects the address pack (BuildEnglish / BuildMultilingual); the
// caller has already fingerprinted the executable. Nothing else about the Phyre
// descriptor is relevant here -- the field fix shares no address with the atlas
// path, only the executable identity behind both.
bool installFieldPhysics(BYTE* base, uint8_t exeBuild);

// Whether DUSK_FIELD_TRACE is set. The trace is useful on its own, so the module
// has to install for it even when neither experimental write is enabled.
bool fieldTraceEnabled();

}  // namespace atfix
