// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

// Stops Escha & Logy and Shallie destroying their own system save data.
//
// THE DEFECT, traced in full in WORK_DOC.md "The system-save wipe", is NOT in
// the writer. It is a load that reports success when it failed, followed by a
// save that writes the resulting defaults over a healthy file. Four independent
// missing checks line up, and any one of them would have prevented it:
//
//   1. PlatformSteam::Load::step routes its _wfopen_s-failure and null-FILE
//      branches into the SAME exit as success, which sets the completion flag --
//      and the store there also clears the error code the constructor set. A
//      transient open failure on an existing, healthy file is indistinguishable
//      from a clean load.
//   2. A zero-byte or short file also reports success: the read is
//      fread(buf, 0x5000, 1, fp), which returns 0 for any real file, so the code
//      recovers the length with ftell and declares success unconditionally.
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
//
// THE CORRECTION is two guards, both of which only ever make the game MORE
// conservative about its own data:
//
//   * After a system-data load, if the object claims completion but read zero
//     bytes, put it into the failure state the engine already has (state 7,
//     error 5, completion cleared). That is the state the read-failure branch
//     four instructions above sets, so nothing downstream sees a situation it
//     was not written to handle -- the caller simply skips the install, and the
//     live system data keeps whatever it already had.
//   * While that has happened, refuse SYSTEM-DATA saves. This is the guard that
//     actually protects the file, because it is the write that would otherwise
//     replace good data with defaults. It is released as soon as any system load
//     succeeds.
//
// GAMEDATA IS DELIBERATELY UNTOUCHED. Load::step and Save::step are shared by
// both save kinds and are told which is which by a flag on the object; every
// path here tests that flag first. Blocking an ordinary game save would be a
// far worse defect than the one being fixed.
//
// All four builds carry the defect byte-for-byte at the shared exit, despite
// Shallie's save layer being refactored.
namespace dusk {

// Installs the guards. Returns true only when both hooks went in. Declines, with
// a logged reason, when the feature is off, the build has no address row, or a
// prologue does not match.
bool installSystemSaveFix(BYTE* base, const atfix::KtglGame& game);

}  // namespace dusk
