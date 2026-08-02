// SPDX-License-Identifier: MIT
#pragma once

namespace atfix {

// Installs a last-chance unhandled-exception filter that writes a post-mortem
// (exception, registers, module+RVA stack scan) to dusk-fix.log. Idempotent;
// DUSK_CRASH_LOG=0 disables it.
//
// Ported unchanged from the Arland project (this project's own code, MIT).
// Brought over the moment a supersampling run died at ~19 seconds leaving
// nothing in the log but healthy counters: with no crash handler, a fault is
// indistinguishable from the process being closed, and every diagnostic this
// mod writes stops one line short of the thing that matters.
void installCrashLogger();

}  // namespace atfix
