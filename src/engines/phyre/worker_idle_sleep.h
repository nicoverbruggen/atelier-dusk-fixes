// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

// Shortens the idle poll of the worker thread a scene transition waits for.
//
// THE DEFECT. Changing game mode runs to completion inside one call on the game
// thread. Part of that work is handed to a short-lived worker, and the game
// thread then joins it with `WaitForSingleObject(INFINITE)`. The worker's loop
// takes a lock, finds nothing to do, releases the lock and calls `Sleep(500)`
// before rechecking the stop flag its shutdown sets. So asking that worker to
// stop costs up to a full half second, and the game thread spends that half
// second parked, computing nothing.
//
// MEASURED ON RORONA, in the sibling Arland project where this was found. A
// battle transition cost 546 to 570 ms, of which one wait accounted for 456 to
// 459 ms across seven sessions, reproducible to within 0.6% because it is a
// constant rather than an amount of work. The whole process burned 20 to 60 ms
// of CPU across it. Shortening the sleep to 10 ms took the transition to 165 to
// 173 ms with every cache counter unchanged.
//
// THE CORRECTION replaces the argument of that one `Sleep`, selected by its
// caller's return address, and touches nothing else. The lock is released
// before the call, so a shorter sleep only makes the worker recheck a flag more
// often; it does not widen any critical section and it does not change what the
// worker does when it has work. That second path has its own sleep, computed
// from the object's own period, and is deliberately left alone.
//
// 10 ms is where the measurements settled. Lower keeps paying, at about a
// microsecond per microsecond, against a proportional rise in how often the
// worker reacquires its lock. `DUSK_WORKER_IDLE_SLEEP=0` restores the game's
// own 500 ms for a comparison run; 1 to 500 sets it instead of the default.
//
// AYESHA ONLY IN THIS PROJECT, because the worker is PhyreEngine's. Searching
// for the six-byte `mov ecx, 0x1f4 / call` sequence returns exactly one hit in
// each of the four PhyreEngine executables and zero in Escha and Shallie.
namespace atfix {

// Installs the override. Declines, with a logged reason, when the feature is
// off or the build does not carry the expected bytes at its row's address.
bool installWorkerIdleSleep(BYTE* base, uintptr_t sleepSiteRva);

}  // namespace atfix
