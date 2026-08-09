// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

// Stops Escha & Logy and Shallie accepting empty save-data loads as successful,
// and stops a failed system-data load from destroying the healthy file later.
//
// THE DEFECT is NOT in the writer. It is a load that reports success when it
// failed, followed by a save that writes the resulting defaults over a healthy
// file. Four independent missing checks line up, and any one of them would have
// prevented it:
//
//   1. PlatformSteam::Load::step routes its _wfopen_s-failure and null-FILE
//      branches into the SAME exit as success, which sets the completion flag --
//      and the store there also clears the error code the constructor set. A
//      transient open failure on an existing, healthy file is indistinguishable
//      from a clean load.
//   2. A zero-byte or short file also reports success: the read is
//      fread(buf, 0x5000, 1, fp), which returns 0 for either, so the code recovers
//      the length with ftell and declares success unconditionally. This guard
//      identifies zero bytes. Rejecting a non-empty truncation also needs the
//      expected complete length, which this object does not carry.
//   3. The system-load thread then copies its zero-filled scratch buffer over
//      the live system-data vector. The codec carries an integrity check, but
//      the caller passes null for the out-size and discards the return value.
//   4. The deserializer accepts an empty blob silently -- a missing chunk just
//      advances to the next reader -- so no error is ever shown.
//
// Then saveSystemData writes the defaulted object back, with no "was this ever
// loaded?" guard anywhere on the path. Its trigger is the Options screen, which
// is why the data is lost only if the player actually changes a setting after a
// failed load. The write path has no safety net either: fopen with L"wb"
// truncates the file at the moment of opening, with no temp file, no rename and
// no backup, and the save runs on a detached thread that is joined only at the
// START of the next request rather than at shutdown -- so a quit taken in that
// window leaves a zero-byte file, which then feeds defect 1 on the next launch.
// The two routes close the loop on each other, which is why it looks random.
// GAMEDATA uses the same load object and false-success exit. It has no automatic
// write-back, but the failed load is still silent and would install an empty
// scratch vector as the live game data if the hook did not force failure.
//
// THE CORRECTION is two guards, both of which only ever make the game MORE
// conservative about its own data:
//
//   * After either kind of load, if the object claims completion but read zero
//     bytes, put it into the failure state the engine already has (state 7,
//     error 5, completion cleared). That is the state the read-failure branch
//     four instructions above sets, so nothing downstream sees a situation it
//     was not written to handle. The caller skips the install, leaving the live
//     data untouched; GAMEDATA also reaches the engine's own Load Error dialog.
//   * After this happens to SYSTEMDATA, refuse SYSTEM-DATA saves. This is the
//     second guard that protects that automatically rewritten file, because the
//     write would otherwise replace good data with defaults. It is released as
//     soon as a system-data load succeeds.
//
// GAMEDATA SAVES remain untouched. Load::step and Save::step are shared by both
// save kinds and are told which is which by a flag on the object. The load guard
// covers both, while the save refusal and its latch cover system data only:
// blocking an ordinary game save would be a worse defect than the one fixed.
//
// All four builds carry the defect byte-for-byte at the shared exit, despite
// Shallie's save layer being refactored.
namespace dusk {

// Installs the guards. Returns true only when both hooks went in. Declines, with
// a logged reason, when the feature is off, the build has no address row, or a
// prologue does not match.
bool installSystemSaveFix(BYTE* base, const atfix::KtglGame& game);

}  // namespace dusk
