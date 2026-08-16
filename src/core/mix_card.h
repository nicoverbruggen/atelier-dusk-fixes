// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>

// Frame-rate-independent synthesis animation.
//
// THE DEFECT. `Card::Update` is the fixed-timestep pump behind the synthesis
// product-card animation, and its loop is BOTTOM-TESTED. The five bytes where a
// pre-test belongs are an alignment NOP:
//
//   cvttss2si ebx, xmm1        ; n = (int)(acc * 59.94)
//   0f 1f 44 00 00             ; <-- padding, NOT a branch
//   <loop top>                 ; body entered unconditionally
//   ...
//   jg <loop top>              ; the only loop control
//
// So it runs max(1, floor(acc * 59.94)) ticks -- never zero. At or below the
// authored 59.94 Hz that is correct. Above it the computed count is always 0
// and the body still runs once, so the tick rate becomes the FRAME rate:
//
//   59.94 Hz -> 59.9 ticks/s   (correct)
//   144   Hz -> 144  ticks/s   (2.40x)
//   200   Hz -> 200  ticks/s   (3.34x)
//
// 200/59.94 = 3.34, which is the ratio reported in game. A corroborating
// symptom: `acc -= step` still fires on every frame whose count was zero, so
// above 60 fps the accumulator drifts unboundedly negative and stops being an
// accumulator at all. What the loop steps is frame-counted -- an integer
// countdown decremented once per tick driving a nine-state machine -- so the
// durations really are authored in frames.
//
// THE CORRECTION is to supply the missing pre-test, and nothing else: decide
// whether a tick is due using the engine's own predicate, and if it is, call the
// original completely untouched. It recomputes the same value from the same
// field and behaves exactly as shipped. If no tick is due, bank the elapsed time
// and return. Nothing is predicted or simulated, which is why this cannot
// desynchronise from whatever else reads the card state; and `MixDemoCard`
// still publishes to the UI node every rendered frame after the pump returns,
// so motion stays smooth at any refresh rate.
//
// SCOPE. The idiom occurs exactly ONCE per executable -- verified against the
// constant pair across all six games; the other hits read the same address as a
// packed `mulps`/`divps` vector lane (NTSC-rate math) rather than a scalar
// accumulator. The function itself is present in all twelve builds of all six
// games with a byte-identical prologue and identical 0x83 size, so one expected
// window covers everything.
//
// `Card` is Gust framework code rather than either engine's, which is why the
// correction lives in core and why all three Dusk games install it:
// `engines/ktgl/ktgl.cpp` supplies the four KTGL rows and
// `engines/phyre/phyre.cpp` Ayesha's two. The Arland mod carries the same
// correction over the other six builds.
//
// The container is deliberately pumped TWICE per frame while the synthesis
// state is running -- once from the game mode and once from the state itself,
// in both KTGL games. That is shipped behaviour at 60 Hz; this fix preserves it,
// because it only ever decides whether the engine's own tick is due. Ayesha was
// never counted the same way, and the fix does not depend on the count for that
// same reason. It has the same shape around the pump: `Card::Update` is in no
// vtable and has one static caller, `MixDemoCard::Update`, which calls it first
// and then publishes to the UI node.
namespace atfix {

// One executable's row. Supplied by the engine module, because address packs do
// not belong in src/core.
struct MixCardTarget {
  uintptr_t updateRva;
  std::array<BYTE, 16> expected;
};

// Installs the fix. Returns true only if the hook went in. Declines, with a
// logged reason, when the feature is off, the row is empty, or the prologue does
// not match.
bool installMixCardFix(BYTE* base, const MixCardTarget& target);

}  // namespace atfix
