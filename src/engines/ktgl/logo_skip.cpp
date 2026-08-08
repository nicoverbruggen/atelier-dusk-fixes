// SPDX-License-Identifier: MIT
//
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
// measured, not guessed: a build that only did the first reached the title
// about three seconds sooner instead of eight, with both logos still visible.
//
// FIRST, THE HOLD. After the shared body has set a step up and zeroed the timer,
// put a large number back. The advance check then finds the step's time
// exhausted. A large number rather than a chosen one because that check compares
// the timer against more than one field of the row, and which float is the hold
// and which is the fade was never established -- a value past all of them ends
// the step whichever way the comparison reads.
//
// SECOND, THE FADES, which is what the first version missed. Each step opens
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
// WHY THIS AND NOT SUPPRESSING THE IMAGE. The first attempt did the latter --
// it forwarded the image loader an out-of-range row so the engine's own bound
// check would hide the layer. It crashed Escha and hung Shallie. The loader does
// have that branch, and rows 0 and 1 of the image table do carry a null name,
// but a branch existing is not the same as a caller surviving it: the consumer
// reads the held row back and does not tolerate the -1 that branch writes. The
// state this fix leaves behind needs no such argument, because it is the state
// every ordinary boot reaches once the logos have played out.
//
// WHAT IT SAVES. The eight seconds of hold in the table, plus the fades around
// them. The "skipping does not start the game sooner" caveat Ayesha carries does
// NOT apply here; it was assumed for these two games until the table was read.
//
// THE HEALTH AND SAFETY NOTICE (step 0) goes with them, being a step of the same
// sequence. Worth knowing: it has never been seen loading -- two traced boots of
// Escha requested rows 5 and 4 and never 6 -- so on some installs there may be
// nothing for that step to skip.
//
// HOW THE SEQUENCE WAS FOUND, because static analysis did not find it. The three
// enter functions are referenced only as 32-bit rip-relative `lea`s inside
// `Title`'s initializer, so no absolute-address search reaches them and
// `callsites` reports none. What identified them was ipu_trace.cpp logging the
// caller of every image load during one boot: both logos came from one call
// site, 1.3 seconds apart.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "logo_skip.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// (Title*, step). The shared body all three logo states call. Declared with a
// return so the detour forwards whatever the original leaves in rax rather than
// assuming the callers ignore it.
using LogoEnterProc = uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, int32_t);

LogoEnterProc originalLogoEnter = nullptr;

// Resolved at install time. The hook runs on the thread the title screen updates
// from, so it must not read the ini or the environment.
std::atomic<bool> skipping{false};

// Byte offset of the elapsed-seconds float within `Title`, per build: Shallie
// lays that object out differently from Escha (0x7c0 against 0x330), which is
// also why the two games' `Ipu` vtables differ in length.
std::atomic<uint32_t> elapsedOffset{0};

// Seconds to claim have already passed. The largest hold in either table is 4.0,
// so this is three orders of magnitude past every threshold while staying
// somewhere float arithmetic is unremarkable: a fade computed as elapsed/hold
// stays a plain number rather than an infinity.
constexpr float kElapsedExhausted = 1000.0f;

std::atomic<int> stepsSkipped{0};

// Byte offset of the step's image object on the `Title`, and the object itself
// once a step has created one. Each step makes a new one, so this is refreshed
// on every enter rather than resolved once.
std::atomic<uint32_t> ipuOffset{0};
std::atomic<uintptr_t> logoIpu{0};
std::atomic<uint32_t> hideOffset{0};

// (Ipu*) -> non-zero while a fade is running on that object.
using FadeBusyProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t);

FadeBusyProc originalFadeBusy = nullptr;

// How many calls a step's answer is allowed to last. A step needs about three
// frames and the predicate is asked once per frame, so this is generous.
//
// IT IS BOUNDED FOR A REASON, and the reason is not tidiness. The step's image
// is destroyed when the step ends, and these games allocate image objects
// constantly, so the address is handed out again quickly. An unbounded rule
// keyed on the pointer would keep answering for whatever object landed there
// next -- telling the engine no fade is running on a menu element that is
// mid-fade, for the rest of the session. Nothing about the pointer itself can
// distinguish the two; only the fact that the step is long over can, and the
// budget is how that gets expressed. Running out degrades to the shipped
// behaviour: the step simply takes its normal time.
constexpr int kFadeAnswerBudget = 16;

std::atomic<int> fadeAnswers{0};

BYTE STDMETHODCALLTYPE quietFadeBusy(uintptr_t self) {
  // Only the logo step's own image, and only while its step is still going.
  // Anything else -- every menu, background and portrait in the game -- goes to
  // the original.
  if (skipping.load(std::memory_order_relaxed) && self &&
      self == logoIpu.load(std::memory_order_relaxed)) {
    if (fadeAnswers.fetch_sub(1, std::memory_order_relaxed) > 0)
      return 0;
    // Spent. Drop the pointer so a later object cannot inherit the answer even
    // if the budget is refilled by the next step.
    logoIpu.store(0, std::memory_order_relaxed);
    fadeAnswers.store(0, std::memory_order_relaxed);
  }
  return originalFadeBusy(self);
}

uintptr_t STDMETHODCALLTYPE skippedLogoEnter(uintptr_t self, int32_t step) {
  // The original runs first and in full: it builds the step's image, stores it
  // on the Title and zeroes the timer. Only then is the timer overwritten, so
  // the write cannot be undone by the reset it is replacing.
  const uintptr_t result = originalLogoEnter(self, step);
  if (!skipping.load(std::memory_order_relaxed))
    return result;

  *reinterpret_cast<float*>(self + elapsedOffset.load(std::memory_order_relaxed))
    = kElapsedExhausted;
  // Read after the original, because the original is what creates it.
  const uintptr_t ipu = *reinterpret_cast<uintptr_t*>(
    self + ipuOffset.load(std::memory_order_relaxed));
  logoIpu.store(ipu, std::memory_order_relaxed);
  fadeAnswers.store(kFadeAnswerBudget, std::memory_order_relaxed);
  // Three frames is fast but not invisible -- the Gust logo was still showing as
  // a flash. Clearing the object's draw byte gives the black screen the step
  // would otherwise have spent its time fading through. The row it holds is left
  // valid, which is the difference between this and the first attempt: the
  // engine's own no-image branch clears this byte AND writes -1 as the row, and
  // it was the -1 that consumers could not survive.
  if (ipu)
    *reinterpret_cast<BYTE*>(ipu + hideOffset.load(std::memory_order_relaxed))
      = 0;
  const int n = stepsSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
  // Three lines a boot, small enough to leave on. It is also what would say the
  // sequence really is three steps, rather than the two that have ever been
  // observed loading an image.
  log("LOGO: step ", std::dec, step, " ended immediately (", n, " so far)");
  return result;
}

// Byte-identical in all four builds; the window carries no rip-relative or
// absolute operand.
//   push rbx / push rsi / push rdi / sub rsp, 0x40 / mov [rsp+0x20], -2
constexpr std::array<BYTE, 16> kLogoEnterExpected = {
  0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x40,
  0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff,
};

// The timer reset at the tail of that body is `mov dword ptr [rbx + disp32],
// esi`. Finding it with the descriptor's offset is what confirms the field about
// to be written is the one this build's own code zeroes, rather than the one
// that was true when the pack was derived.
constexpr std::array<BYTE, 2> kTimerResetOpcode = { 0x89, 0xb3 };

// The fade predicate, one shape per game and shared by that game's two builds --
// the arrangement system_save_fix.cpp already uses. Escha reads a float off the
// object directly; Shallie reads a pointer first, which is why the two cannot
// share a window and why the offset is never assumed to be the same.
//   Escha:   movss xmm1, [rcx+0xdc] / xor al, al / xorps xmm0, xmm0 / mov edx, 1
//   Shallie: mov rdx, [rcx+0x48]    / xor al, al / xorps xmm0, xmm0 / mov ecx, 1
constexpr std::array<BYTE, 16> kFadeBusyEschaExpected = {
  0xf3, 0x0f, 0x10, 0x89, 0xdc, 0x00, 0x00, 0x00,
  0x32, 0xc0, 0x0f, 0x57, 0xc0, 0xba, 0x01, 0x00,
};
constexpr std::array<BYTE, 16> kFadeBusyShallieExpected = {
  0x48, 0x8b, 0x51, 0x48, 0x32, 0xc0, 0x0f, 0x57,
  0xc0, 0xb9, 0x01, 0x00, 0x00, 0x00, 0xf3, 0x0f,
};
constexpr size_t kLogoEnterScan = 0xe0;   // both bodies are 0xdb..0xde long

bool bodyZeroesField(const BYTE* body, uint32_t offset) {
  for (size_t i = 0; i + 6 <= kLogoEnterScan; ++i) {
    if (std::memcmp(body + i, kTimerResetOpcode.data(), 2) != 0)
      continue;
    uint32_t disp = 0;
    std::memcpy(&disp, body + i + 2, sizeof(disp));
    if (disp == offset)
      return true;
  }
  return false;
}

}  // namespace

bool installKtglLogoSkip(BYTE* base, const KtglGame& game) {
  if (featureSupport(Feature::SkipStartupLogos) == Support::Unsupported) {
    log("FIXES logo_skip=not_applicable");
    return false;
  }
  if (!featureEnabled(Feature::SkipStartupLogos)) {
    log("FIXES logo_skip=off");
    return false;
  }
  if (!game.logoEnterRva || !game.logoElapsedOffset) {
    log("FIXES logo_skip=no_row_for_this_build");
    return false;
  }

  BYTE* target = base + game.logoEnterRva;
  if (!matches(target, kLogoEnterExpected)) {
    log("Logo-skip prologue mismatch at 0x", std::hex, game.logoEnterRva,
        std::dec, "; not installing");
    return false;
  }
  if (!bodyZeroesField(target, game.logoElapsedOffset)) {
    log("FIXES logo_skip=declined (0x", std::hex, game.logoEnterRva,
        " does not zero +0x", game.logoElapsedOffset,
        ", so that field is not this build's timer)", std::dec);
    return false;
  }

  if (!game.logoIpuOffset || !game.ipuFadeBusyRva || !game.ipuHideOffset) {
    log("FIXES logo_skip=no_row_for_this_build (fade predicate)");
    return false;
  }
  BYTE* fadeTarget = base + game.ipuFadeBusyRva;
  const auto& fadeExpected = currentTitle() == Title::Shallie
    ? kFadeBusyShallieExpected : kFadeBusyEschaExpected;
  if (!matches(fadeTarget, fadeExpected)) {
    log("Logo-skip fade-predicate prologue mismatch at 0x", std::hex,
        game.ipuFadeBusyRva, std::dec, "; not installing");
    return false;
  }

  elapsedOffset.store(game.logoElapsedOffset, std::memory_order_relaxed);
  ipuOffset.store(game.logoIpuOffset, std::memory_order_relaxed);
  hideOffset.store(game.ipuHideOffset, std::memory_order_relaxed);

  // The predicate first. With only this one live it answers for a null object
  // that no step ever creates, so nothing is affected -- whereas the enter hook
  // alone is the half-measure that shipped and underdelivered.
  if (!installMinHookDetour(fadeTarget, reinterpret_cast<void*>(&quietFadeBusy),
                            reinterpret_cast<void**>(&originalFadeBusy))) {
    log("FIXES logo_skip=failed (fade predicate)");
    return false;
  }
  if (!installMinHookDetour(target, reinterpret_cast<void*>(&skippedLogoEnter),
                            reinterpret_cast<void**>(&originalLogoEnter))) {
    log("FIXES logo_skip=failed (enter); nothing is skipped");
    return false;
  }

  skipping.store(true, std::memory_order_relaxed);
  log("FIXES logo_skip=active enter=0x", std::hex, game.logoEnterRva,
      " elapsed=+0x", game.logoElapsedOffset, " ipu=+0x", game.logoIpuOffset,
      " fade=0x", game.ipuFadeBusyRva, std::dec);
  return true;
}

}  // namespace atfix
