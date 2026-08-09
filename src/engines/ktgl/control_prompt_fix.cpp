// SPDX-License-Identifier: MIT
//
// See control_prompt_fix.h for the behaviour and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "control_prompt_fix.h"
#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using UpdateProc = void (STDMETHODCALLTYPE*)(uintptr_t, float);
using DrawProc = void (STDMETHODCALLTYPE*)(uintptr_t);

UpdateProc originalUpdate = nullptr;
DrawProc originalDraw = nullptr;

// The value the original clamps dt to (`minss xmm10, 0.1`). Passing exactly this
// makes the ease step the whole remaining distance, so a pane lands on its
// target in one frame. Passing MORE would be pointless: the clamp caps it here.
constexpr float kInstantDt = 0.1f;

std::atomic<uint32_t> g_updates{0};

// Two distinct requests hide in "remove it", and they want different code, so
// there are two settings rather than one tri-state. Keeping both as plain
// booleans is what lets them go through the existing configuration surface
// unchanged -- featureEnabled owns the first, duskConfigBool the second.
//
//   hold  -- the panel appears in place and holds still (the default)
//   hide  -- the panel is not drawn at all
//
// Hide is the bigger behavioural change and removes information a player may be
// relying on, so it is off unless asked for, and asking for it implies hold.
enum class Mode : uint8_t { Off, Hold, Hide };

Mode resolveMode() {
  if (!featureEnabled(Feature::ControlPromptHold))
    return Mode::Off;
  return duskConfigBool("Interface", "HideControlPrompt", false)
    ? Mode::Hide : Mode::Hold;
}

Mode mode() {
  static const Mode resolved = resolveMode();
  return resolved;
}

void STDMETHODCALLTYPE tracedUpdate(uintptr_t self, float dt) {
  // Hide still runs Update, deliberately. It owns the shade node's visibility
  // and clears the drawn-this-frame guard; skipping it leaves a translucent bar
  // on screen and wedges Draw permanently off.
  const Mode m = mode();
  originalUpdate(self, m == Mode::Off ? dt : kInstantDt);
  g_updates.fetch_add(1, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE tracedDraw(uintptr_t self) {
  if (mode() == Mode::Hide)
    return;
  originalDraw(self);
}

// Byte-identical between the English and multilingual Shallie builds: one
// compile of one source, and homolog reports MATCH with identical size 0x7aa.
constexpr std::array<BYTE, 16> kUpdateExpected = {
  0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x55,
  0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
};

// Also byte-identical between both Shallie builds. Hide needs this second hook,
// so it is verified before either hook is enabled; otherwise a wrong/missing
// Draw row could silently degrade the requested Hide mode into Hold.
constexpr std::array<BYTE, 16> kDrawExpected = {
  0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9,
  0x38, 0x0d, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9,
};

}  // namespace
}  // namespace atfix

namespace dusk {

bool installControlPromptFix(BYTE* base, const atfix::KtglGame& game) {
  using namespace atfix;
  auto& log = atfix::log;

  const Mode m = mode();
  if (m == Mode::Off) {
    log("FIXES control_prompt=off");
    return false;
  }
  if (!game.controlPromptUpdateRva) {
    // The honest state for all four non-Shallie rows: Escha does not have this
    // panel at all.
    log("FIXES control_prompt=unavailable (this executable has no control-hint"
        " panel)");
    return false;
  }

  BYTE* update = base + game.controlPromptUpdateRva;
  if (!matches(update, kUpdateExpected)) {
    log("FIXES control_prompt=declined (prologue mismatch)");
    return false;
  }

  BYTE* draw = game.controlPromptDrawRva
    ? base + game.controlPromptDrawRva : nullptr;
  if (m == Mode::Hide && (!draw || !matches(draw, kDrawExpected))) {
    log("FIXES control_prompt=declined (draw prologue mismatch or missing)"
        " mode=hide");
    return false;
  }

  HookTransaction transaction;
  bool created = transaction.create(update,
    reinterpret_cast<void*>(&tracedUpdate),
    reinterpret_cast<void**>(&originalUpdate));
  // Draw is only needed by hide, but when it is needed both hooks are one
  // transaction. A failed Draw create/enable can no longer leave Update live
  // and turn the user's Hide request into Hold for the rest of the process.
  if (created && m == Mode::Hide)
    created = transaction.create(draw, reinterpret_cast<void*>(&tracedDraw),
      reinterpret_cast<void**>(&originalDraw));
  const bool enabled = created && transaction.enableAll();
  if (!enabled) {
    const HookTransactionFailure failure = transaction.failure();
    log("CONTROL_PROMPT transaction failed stage=",
        hookTransactionStageName(failure.stage), " status=", failure.status);
    if (!transaction.rollback()) {
      const HookTransactionFailure rollback = transaction.rollbackFailure();
      log("CONTROL_PROMPT rollback_incomplete stage=",
          hookTransactionStageName(rollback.stage), " status=",
          rollback.status);
    }
  } else {
    transaction.commit();
  }

  log("FIXES control_prompt=", enabled ? "active" : "failed",
      " mode=", m == Mode::Hide ? "hide" : "hold",
      " update_rva=0x", std::hex, game.controlPromptUpdateRva, std::dec);
  return enabled;
}

}  // namespace dusk
