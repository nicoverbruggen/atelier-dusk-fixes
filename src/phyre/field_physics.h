// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

#include "../core/hook_util.h"

namespace atfix {

// The Ayesha field-jitter fix: a port of Arland's threshold rescale and resting
// stabilizer, confirmed in game and on by default. The anchor addresses are
// verified, and so is the live controller layout the stabilizer writes through:
// every offset was checked against both Ayesha builds' controller update.
//
// The two halves are one fix. The stabilizer needs the rescale and refuses to
// run without it, so turning the rescale off turns both off.
//
// DUSK_FIELD_TRACE=1 logs the carried-over controller fields around each
// ground-contact change, and installs on its own without either fix. It is the
// instrument for any further Ayesha-specific analysis.
//
// DUSK_FIELD_ENGINE_FIX overrides the threshold rescale.
// DUSK_FIELD_STABILIZER overrides the controller-object writes and requires the
// rescale. See WORK_DOC.md, "The field-jitter fix".
// `exeBuild` selects the address pack (BuildEnglish / BuildMultilingual); the
// caller has already fingerprinted the executable. Nothing else about the Phyre
// descriptor is relevant here -- the field fix shares no address with the atlas
// path, only the executable identity behind both.
bool installFieldPhysics(BYTE* base, uint8_t exeBuild);

// Whether DUSK_FIELD_TRACE is set. The trace is useful on its own, so the module
// has to install for it even when neither experimental write is enabled.
bool fieldTraceEnabled();

}  // namespace atfix
