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
//      A zero-byte or short file also reports success: the read is
//      fread(buf, 0x5000, 1, fp), which returns 0 for either, so the code recovers
//      the length with ftell and declares success unconditionally.
//   2. A NON-EMPTY truncation therefore passes a byte-count test, and the object
//      carries no expected complete length to compare against. Measured rather
//      than argued: a 64-byte SYSDATA.pcsave loads "successfully", the game runs
//      on defaults without saying anything, and changing one setting writes those
//      defaults over the file. The codec is the only thing that knows better.
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
// THE CORRECTION is three guards, all of which only ever make the game MORE
// conservative about its own data:
//
//   * After either kind of load, if the object claims completion but read zero
//     bytes, put it into the failure state the engine already has (state 7,
//     error 5, completion cleared). That is the state the read-failure branch
//     four instructions above sets, so nothing downstream sees a situation it
//     was not written to handle. The caller skips the install, leaving the live
//     data untouched; GAMEDATA also reaches the engine's own Load Error dialog.
//   * After this happens to SYSTEMDATA, refuse SYSTEM-DATA saves -- but only
//     when there is a file worth refusing for. This is the second guard that
//     protects that automatically rewritten file, because the write would
//     otherwise replace good data with defaults. It is released as soon as a
//     system-data load succeeds.
//
//     WHY IT ASKS THE FILESYSTEM FIRST. A failed open and an empty file arrive
//     at the same exit having read nothing, and they deserve opposite answers.
//     A healthy file behind a transient open failure is the whole reason the
//     refusal exists. An empty file has nothing to protect, and refusing to
//     write it strands the player for good: the latch clears only on a load
//     that reads content, and the refused save is the only thing that could
//     give the file any. That state was measured, not argued -- a zero-byte
//     SYSDATA.pcsave armed the latch at boot, the refusal fired about a second
//     later without the player touching anything, and the file stayed empty.
//     So the guard reads the path the load opened, inline at object+0x18 in all
//     four builds, and arms only when a file with content is actually there.
//     Anything it cannot determine answers "protect", which is the safe way to
//     be wrong.
//
//     This also means the engine's own zero-byte producer is recoverable. The
//     save thread is joined at the start of the next request rather than at
//     shutdown, so a quit inside that window leaves an empty file; the game can
//     now rewrite it instead of being locked out of it.
//   * When the CODEC reports a failed DECODE, say so in the log and do nothing
//     else. That covers the non-empty truncation in defect 2, which no length
//     test can see: the codec returns non-zero on success and writes a sentinel
//     into its destination, and on failure it zeroes that destination and
//     returns zero, and the game throws the answer away. Its fifth argument is
//     the direction, so this looks at reads only.
//
//     REPORTING ONLY, DELIBERATELY. Refusing the save here was built and then
//     removed. The two failures are not the same: a load that read NOTHING may
//     have a healthy file behind it, which is what the refusal above exists to
//     protect, while a failed decode means the bytes are bad and there is no
//     good file left. Refusing then keeps an unreadable file forever and the
//     player gets settings that silently reset every launch, because the game
//     would otherwise have replaced it. Partial files are the expected source,
//     since the write path has no temp file or rename, so letting the game
//     rewrite IS the recovery. A transient short read of a healthy file would
//     also land here and would deserve the refusal, but it cannot be told apart
//     from a short file -- ftell lands at end-of-file either way.
//
// GAMEDATA SAVES remain untouched. Load::step and Save::step are shared by both
// save kinds and are told which is which by a flag on the object. The load guard
// covers both, while the save refusal and its latch cover system data only:
// blocking an ordinary game save would be a worse defect than the one fixed.
// The codec guard changes no behaviour at all, so it cannot refuse anything of
// either kind. It exists because this failure is otherwise completely silent.
//
// AND IT TELLS THE PLAYER, on the English builds only. Neither game does this
// usefully on its own: Escha & Logy says nothing at all and the player finds out
// when their settings are gone, while Shallie shows a box built from UTF-8
// Japanese passed to MessageBoxA, so it renders as mojibake with an `err: -3`.
// Both games import MessageBoxA and neither imports the W form, which is the
// cause. Ours goes out through MessageBoxW, on its own thread -- a modal blocks
// whichever thread shows it, and the decode runs on the load path, so showing it
// inline could leave the game waiting on a load that never finishes.
//
// English only because the message is written once, in English, and a guessed
// translation shown to a Japanese or Chinese player would be worse than what the
// game already does.
//
// On those same builds the game's own box for this failure is suppressed, so the
// player sees one dialog rather than two. Matched narrowly: the executables carry
// five of these boxes (`save` and `load` captions across user and system data),
// and only the system LOAD one has caption "load" with text beginning "system ".
// The suppression is armed only while our dialog is up, so a system-load failure
// this fix did NOT report still shows the game's own box rather than vanishing.
// It is also not required -- if the detour will not install, both dialogs appear
// and the log says so, because a readable message protects no data and must not
// be able to take the guards down with it.
//
// HOOKING THE CODEC IS SCOPED, not a blanket intercept. It has exactly four call
// sites per executable and all four are the save/load consumers, so nothing else
// in the game reaches it. It is byte-identical in all four builds (same 0x832
// size, same prologue, homolog confirming each pair in both directions).
//
// All four builds carry the defect byte-for-byte at the shared exit, despite
// Shallie's save layer being refactored.
namespace dusk {

// Installs the guards. Returns true only when all three hooks went in. Declines, with
// a logged reason, when the feature is off, the build has no address row, or a
// prologue does not match.
bool installSystemSaveFix(BYTE* base, const atfix::KtglGame& game);

}  // namespace dusk
