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

  const bool updateOk = installMinHookDetour(update,
    reinterpret_cast<void*>(&tracedUpdate),
    reinterpret_cast<void**>(&originalUpdate));

  // Draw is only needed by the hide mode, and only hooked for it: hold has no
  // business intercepting a call it does not change.
  bool drawOk = true;
  if (updateOk && m == Mode::Hide && game.controlPromptDrawRva) {
    drawOk = installMinHookDetour(base + game.controlPromptDrawRva,
      reinterpret_cast<void*>(&tracedDraw),
      reinterpret_cast<void**>(&originalDraw));
  }

  log("FIXES control_prompt=", updateOk && drawOk ? "active" : "failed",
      " mode=", m == Mode::Hide ? "hide" : "hold",
      " update_rva=0x", std::hex, game.controlPromptUpdateRva, std::dec);
  return updateOk && drawOk;
}

}  // namespace dusk
