// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktgl.h"

// Stops Shallie's on-screen control-hint panel replaying its slide-in animation.
//
// WHAT IT IS. The panel is twenty text panes created once, parked off the right
// edge of the 1280x720 authoring canvas, easing leftwards into place while a
// shade backdrop grows behind them. It is not an engine animation object: there
// is no SCL animator involved, and `CPaneGroupArrayAnimator` -- the obvious
// suspect by name -- appears only in an unreferenced string pool. The whole
// thing is a hand-rolled immediate-mode easing loop inside ButtonHelp::Update.
//
// THE COMPLAINT is that it replays constantly. The panes are never destroyed,
// so within one lifetime of the object this is re-animation rather than
// recreation -- and because the slot contents are a pure function of prompt kind
// and key bindings, a dialogue advance cannot change them. The likeliest trigger
// is the containing UI manager being reconstructed per game mode or event step,
// which re-parks all twenty panes. That is NOT yet confirmed, and this fix does
// not depend on which it is: it makes the slide finish instantly however often
// it starts.
//
// THE CORRECTION is to hand the original a larger delta time. dt is consumed at
// exactly one instruction -- `minss xmm10, xmm1`, clamping it to 0.1 -- and the
// ease step is |target - x| * 10 * that value. At 0.1 the step is the entire
// remaining distance, so every pane lands exactly on its target in one frame,
// using the game's own overshoot guards. Nothing is patched, no state is forced
// that the object could not have produced itself, and the panel still appears
// and disappears exactly when it did.
//
// WHY NOT SIMPLY SKIP Update, which would be simpler: two things break. The
// shade node is declared visible="true" in the layout XML and only Update ever
// hides it, so an empty translucent bar would sit on screen forever. And the
// drawn-this-frame guard is set by Draw and cleared only by Update, so it would
// stick and turn Draw into a permanent no-op after one frame. Skipping Draw
// instead is coherent, and is the second mode below.
//
// SHALLIE ONLY. Escha & Logy has no equivalent -- no Saves/ui tree at all, an
// older Data/WinXls UI pipeline, pre-composed help lines instead of per-slot
// icon tables, and homolog returns MISMATCH. This is one of several places where
// the two KTGL games are NOT the same code.
//
// A PREFERENCE, NOT A CORRECTION, so under the house rule it gets an ini key and
// ships off by default: suppressing a shipped UI behaviour is a matter of taste
// in a way that a frame-rate-coupled cursor is not.
namespace dusk {

// Installs the suppression. Returns true only if the hook went in. Declines,
// with a logged reason, when the feature is off, the build has no address row
// (every non-Shallie build), or the prologue does not match.
bool installControlPromptFix(BYTE* base, const atfix::KtglGame& game);

}  // namespace dusk
