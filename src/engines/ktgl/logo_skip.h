// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

// Startup logo skip for Escha & Logy and Shallie.
//
// NOT A PORT of the Ayesha one, but the same idea: let the sequence believe it
// has finished and let the engine do the advancing. Ayesha has a logo object
// whose phase field two waiters poll, and its skip writes the terminal phase.
// These two have no such object -- neither executable carries a `Logo`,
// `EasyRender` or `ThreadEasy` type descriptor -- but the title screen owns an
// equivalent: a three-step sequence with an elapsed-seconds field that its own
// advance check reads.
//
// THE SEQUENCE, out of the executable's own data. `Title` builds a small state
// machine, and three of its states are the logos. Each has its own enter
// function and all three call one shared body, which picks a row out of a table
// of `{ u32 imageRow, float, float }` -- identical in all four builds:
//
//     step 0   row 6   warning_text.g1t   1.0   4.0
//     step 1   row 5   logo_kt.g1t        0.0   2.0
//     step 2   row 4   logo_gust.g1t      0.0   2.0
//
// That shared body ends by zeroing the elapsed-seconds field. Each state's own
// update then accumulates the frame time into it and calls the advance check,
// which compares it against the floats above and moves to the next state.
//
// THE SKIP HAS TWO HALVES, and the first one alone is not enough. That was
// measured, not guessed: with only the hold installed the title arrives
// about three seconds sooner instead of eight, with both logos still visible.
//
// FIRST, THE HOLD. After the shared body has set a step up and zeroed the timer,
// put a large number back. The advance check then finds the step's time
// exhausted. A large number rather than a chosen one because that check compares
// the timer against more than one field of the row, and which float is the hold
// and which is the fade was never established -- a value past all of them ends
// the step whichever way the comparison reads.
//
// SECOND, THE FADES, which the hold on its own does not touch. Each step opens
// with a fade in and, once its hold expires, asks for a half-second fade out.
// The state's update begins by asking the image object whether a fade is
// running and returns without doing anything at all while one is, so removing
// the hold just leaves the fades pacing the sequence. The skip therefore also
// answers that question with no.
//
// It answers it for ONE OBJECT and for A BOUNDED NUMBER OF CALLS: the image the
// logo step just created, whose address the enter hook reads off the `Title`
// immediately after the original returns, and then only for as long as the step
// plausibly lasts. Every other image in the game answers for itself.
//
// Both halves of that are necessary. Without the pointer the feature would be a
// global claim that no fade is ever running. Without the budget the pointer
// would outlive the object it names -- the step's image is destroyed when the
// step ends, these games allocate images constantly, and the next object at that
// address would inherit the answer for the rest of the session.
//
// With both halves the step ends in about three frames: one to notice the hold
// is spent and request the fade out, one for the flag the update sets when a
// faded-out step is finished, and one to transition.
//
// WHY THIS AND NOT SUPPRESSING THE IMAGE. Suppressing it means forwarding the
// image loader an out-of-range row so the engine's own bound check hides the
// layer. That crashes Escha and hangs Shallie. The loader does
// have that branch, and rows 0 and 1 of the image table do carry a null name,
// but a branch existing is not the same as a caller surviving it: the consumer
// reads the held row back and does not tolerate the -1 that branch writes. The
// state this fix leaves behind needs no such argument, because it is the state
// every ordinary boot reaches once the logos have played out.
//
// WHAT IT SAVES. The eight seconds of hold in the table, plus the fades around
// them. The "skipping does not start the game sooner" caveat Ayesha carries does
// NOT apply here.
//
// THE HEALTH AND SAFETY NOTICE (step 0) goes with them, being a step of the same
// sequence. Worth knowing: it has never been seen loading -- two traced boots of
// Escha requested rows 5 and 4 and never 6 -- so on some installs there may be
// nothing for that step to skip.
//
// HOW THE SEQUENCE WAS FOUND, because static analysis did not find it. The three
// enter functions are referenced only as 32-bit rip-relative `lea`s inside
// `Title`'s initializer, so no absolute-address search reaches them and
// `callsites` reports none. What identified them was an instrumented boot that
// logged the caller of every image load: both logos came from one call site,
// 1.3 seconds apart.
namespace atfix {

// Skip the Gust and Koei Tecmo logo images shown on the way to the title
// screen. Installs only when the capability matrix supports the feature and the
// user opted in. Returns true when the hook is live and both logo rows were
// found in the running executable's own image table.
bool installKtglLogoSkip(BYTE* base, const KtglGame& game);

}  // namespace atfix
