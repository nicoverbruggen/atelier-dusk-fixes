// SPDX-License-Identifier: MIT
//
// Startup logo skip for Ayesha.
//
// Ported from the Arland project's src/logo_skip.cpp, which is this project's
// own code (MIT). The mechanism is identical because the code is: Ayesha runs
// the same PhyreEngine boot sequence, and both hooked functions are
// byte-identical to the Arland ones over their prologue windows.
//
// The boot logos are not part of the title-screen state machine. They belong to
// ThreadEasyRenderLogo, a small object the application creates before it starts
// initialising the engine. Its update runs on the render thread and steps a
// phase sequence over fullscreen picture layers. The application does not wait
// for the logos before loading: it creates the object, performs the whole engine
// and resource initialisation while the render thread animates, and only then
// spins until the sequence reports its terminal phase. A separate title-side
// player blocks on the same object for the attract replay after an idle title
// screen. Both wait on nothing but the phase field, so writing the terminal
// phase releases both.
//
// TWO CONSEQUENCES A READER SHOULD EXPECT, both confirmed in Ayesha's own boot
// function rather than carried over from Arland:
//
//   Skipping does not start the game sooner. The logos play while the game
//   loads, so this shows the clear colour for as long as loading genuinely
//   takes. That is the honest presentation and it is not a defect.
//
//   The idle-title attract replay stops happening, because the second waiter
//   polls the same field.
//
// Two hooks, because one is not enough to guarantee a clean screen. Forcing the
// phase stops the sequence advancing, but the picture layers are already
// constructed and their alpha only reaches the material when the layer's own
// update runs, which the forced path no longer calls. Rather than reason about
// what a never-ticked layer draws, the draw is suppressed as well.
//
// The object is left structurally intact, so the game's own destructor still
// frees the picture layers.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "logo_skip.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// ThreadEasyRenderLogo::update(float) and ::draw(context), vtable slots 1 and
// 2. Both return a BYTE-wide true in every path.
using LogoUpdateProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t, float);
using LogoDrawProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t);

// THESE ARE THE FUNCTION BODIES, NOT THE VTABLE SLOTS. Ayesha is an
// incremental-link build, so every vtable entry holds a `jmp` thunk rather than
// the function. Each address below was taken from that build's own RTTI vtable
// for ThreadEasyRenderLogo and then followed through its thunk. Hooking a thunk
// would catch only the calls that route through that one thunk; hooking the
// body catches every route.
//
// EN vtable 0xcf1760, ML vtable 0xcf4f60, both from their own build's RTTI. The
// multilingual pair is additionally a `homolog` MATCH from the English one with
// the reverse vote confirming.
struct LogoRvas {
  uintptr_t update;
  uintptr_t draw;
};
constexpr LogoRvas kLogoRvas[2] = {
  /* BuildEnglish      */ { 0x28aa90, 0x28ad90 },
  /* BuildMultilingual */ { 0x292ad0, 0x292dd0 },
};

// The phase field the two waiters poll, and the value that ends the sequence.
// Phase 5 is the terminal state: the original's own dispatch falls through it
// without touching anything. Read out of both Ayesha builds' update functions
// (`mov dword ptr [rbx+0x20], 5` at EN 0x28ab44 / ML 0x292b84), not inherited.
constexpr uintptr_t kPhaseOffset = 0x20;
constexpr int32_t kPhaseFinished = 5;

// Byte-identical in the English and multilingual executables, and byte-identical
// to the Arland project's own windows for the same two functions.
constexpr std::array<BYTE, 16> kLogoUpdateExpected = {
  0x48, 0x8b, 0xc4, 0x57, 0x48, 0x83, 0xec, 0x60,
  0x48, 0xc7, 0x40, 0xc8, 0xfe, 0xff, 0xff, 0xff,
};
constexpr std::array<BYTE, 16> kLogoDrawExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
  0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48,
};

LogoUpdateProc originalLogoUpdate = nullptr;
LogoDrawProc originalLogoDraw = nullptr;

// Resolved once at install time. The hooks run on the render thread, so they
// must not touch the ini or the environment; reading a plain flag keeps them
// free of both.
std::atomic<bool> skipping{false};

BYTE STDMETHODCALLTYPE skippedLogoUpdate(uintptr_t self, float elapsed) {
  if (!skipping.load(std::memory_order_relaxed))
    return originalLogoUpdate(self, elapsed);
  *reinterpret_cast<int32_t*>(self + kPhaseOffset) = kPhaseFinished;
  return 1;
}

BYTE STDMETHODCALLTYPE skippedLogoDraw(uintptr_t self, uintptr_t context) {
  if (!skipping.load(std::memory_order_relaxed))
    return originalLogoDraw(self, context);
  return 1;
}

}  // namespace

bool installLogoSkip(BYTE* base, uint8_t exeBuild) {
  if (featureSupport(Feature::SkipStartupLogos) == Support::Unsupported) {
    log("FIXES logo_skip=not_applicable");
    return false;
  }
  if (!featureEnabled(Feature::SkipStartupLogos)) {
    log("FIXES logo_skip=off");
    return false;
  }

  const LogoRvas& rvas =
    kLogoRvas[exeBuild == BuildEnglish ? 0 : 1];
  BYTE* updateTarget = base + rvas.update;
  BYTE* drawTarget = base + rvas.draw;

  if (!matches(updateTarget, kLogoUpdateExpected) ||
      !matches(drawTarget, kLogoDrawExpected)) {
    log("Logo-skip prologue mismatch update=0x", std::hex, rvas.update,
        " draw=0x", rvas.draw, std::dec, "; not installing");
    return false;
  }

  // Install the draw suppression first: on a partial install the sequence still
  // runs and still draws, which is the shipped behaviour, rather than a stopped
  // sequence whose stale picture layers are left on screen.
  if (!installMinHookDetour(drawTarget,
                            reinterpret_cast<void*>(&skippedLogoDraw),
                            reinterpret_cast<void**>(&originalLogoDraw))) {
    log("FIXES logo_skip=failed (draw)");
    return false;
  }
  if (!installMinHookDetour(updateTarget,
                            reinterpret_cast<void*>(&skippedLogoUpdate),
                            reinterpret_cast<void**>(&originalLogoUpdate))) {
    log("FIXES logo_skip=failed (update)");
    return false;
  }

  skipping.store(true, std::memory_order_relaxed);
  log("FIXES logo_skip=active update=0x", std::hex, rvas.update,
      " draw=0x", rvas.draw, std::dec);
  return true;
}

}  // namespace atfix
