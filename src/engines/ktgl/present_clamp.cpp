// SPDX-License-Identifier: MIT
//
// The present-size clamp, and KTGL's answers to supersample_policy.h. See
// present_clamp.h for the run this came out of and why it clamps the swap chain
// rather than writing the engine's own override fields.

#include "present_clamp.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/highres.h"
#include "../../core/log.h"
#include "../../core/supersample.h"

namespace atfix {
extern Log log;   // main.cpp
}

namespace atfix {
namespace {

// The size to present at, resolved once and cached.
unsigned int g_clampWidth = 0;
unsigned int g_clampHeight = 0;

// Keep this final consumer inside the same ceiling as the launcher and the
// shared supersampling policy. The launcher normally guarantees it, but the
// ini remains an editable input at DLL load time.
constexpr unsigned int kMaxConfiguredWidth = 7680;
constexpr unsigned int kMaxConfiguredHeight = 4320;

// What the engine asked for before being clamped. Read by window_size.cpp, which
// has to recognise the window the engine sizes from this same number.
std::atomic<UINT> g_clampedRenderWidth{0};
std::atomic<UINT> g_clampedRenderHeight{0};

// TOLD, NOT GUESSED. The launcher writes the base resolution the user chose into
// `[Rendering] DisplayWidth`/`DisplayHeight` and multiplies the game's own
// Setting.ini by the supersampling factor. The launcher produces the pair
// together, while this final consumer still validates it because the ini can be
// edited independently. That is the same split the Arland launcher has -- base
// resolution plus a multiplier -- and the same key names, so a reader moving
// between the two mods finds the same file.
//
// Deriving the base by dividing the requested size by the factor would look
// simpler and is not: the launcher truncates and masks to an even size, so the
// division does not always invert, and a 1.5x factor is exactly where it stops
// being exact.
//
// The desktop is the fallback, for the env-only experimental path where nobody
// wrote the keys. Read at DEVICE CREATION rather than lazily, because once the
// game is fullscreen the metrics report the mode it asked for rather than the
// desktop -- which is the very size being clamped away.
bool displaySize(unsigned int* width, unsigned int* height) {
  static const bool have = [] {
    const int configuredWidth = duskConfigInt("Rendering", "DisplayWidth", 0);
    const int configuredHeight = duskConfigInt("Rendering", "DisplayHeight", 0);
    if (configuredWidth > 0 && configuredHeight > 0 &&
        unsigned(configuredWidth) <= kMaxConfiguredWidth &&
        unsigned(configuredHeight) <= kMaxConfiguredHeight) {
      g_clampWidth = unsigned(configuredWidth);
      g_clampHeight = unsigned(configuredHeight);
      return true;
    }
    if (configuredWidth || configuredHeight)
      log("SSAA present clamp: invalid [Rendering] DisplayWidth/DisplayHeight ",
          std::dec, configuredWidth, "x", configuredHeight,
          " ignored (both must be positive and at most ",
          kMaxConfiguredWidth, "x", kMaxConfiguredHeight, ")");
    const int cx = GetSystemMetrics(SM_CXSCREEN);
    const int cy = GetSystemMetrics(SM_CYSCREEN);
    if (cx <= 0 || cy <= 0)
      return false;
    g_clampWidth = unsigned(cx);
    g_clampHeight = unsigned(cy);
    log("SSAA present clamp: no [Rendering] DisplayWidth/DisplayHeight in the"
        " ini, falling back to the desktop size ", std::dec, g_clampWidth, "x",
        g_clampHeight);
    return true;
  }();
  if (!have)
    return false;
  *width = g_clampWidth;
  *height = g_clampHeight;
  return true;
}

void clampPresentSize(UINT* width, UINT* height, const char* where) {
  if (!ktglPresentClampEnabled() || !width || !height)
    return;
  unsigned int displayWidth = 0, displayHeight = 0;
  if (!displaySize(&displayWidth, &displayHeight))
    return;
  // Only ever downwards, and only when the game asked for more than the panel
  // has. A game already at or below the display size is not supersampling and
  // must not be touched.
  if (*width <= displayWidth && *height <= displayHeight)
    return;
  const UINT wasWidth = *width;
  const UINT wasHeight = *height;
  g_clampedRenderWidth.store(wasWidth, std::memory_order_relaxed);
  g_clampedRenderHeight.store(wasHeight, std::memory_order_relaxed);
  // Clamp each component independently. A hand-edited mismatched pair must
  // never make the other component larger than the game requested.
  *width = std::min(wasWidth, UINT(displayWidth));
  *height = std::min(wasHeight, UINT(displayHeight));
  // Once per distinct call site and size, so a per-frame resize cannot flood the
  // log while a one-off still gets recorded.
  static std::atomic<uint64_t> reported{0};
  const uint64_t key = (uint64_t(wasWidth) << 32) | wasHeight;
  if (reported.exchange(key, std::memory_order_relaxed) != key)
    log("SSAA present clamp: ", where, " ", std::dec, wasWidth, "x", wasHeight,
        " -> ", *width, "x", *height,
        " (the engine keeps rendering at the larger size; its own device init"
        " should now take the offscreen branch)");
}

// Only the resize. The line reporting the window against the back buffer stays
// in core and runs on both routes, because "the window is not the size of the
// back buffer" is the one fact that separates a composite drawn wrongly from a
// window that was never the right size, and guessing between those two has
// already cost a round trip on the engine that has no clamp.
void fitOutputWindow(const DXGI_SWAP_CHAIN_DESC* desc) {
  if (!desc || !desc->OutputWindow)
    return;
  if (!ktglPresentClampEnabled() || !desc->Windowed)
    return;
  const UINT clientWidth = desc->BufferDesc.Width;
  const UINT clientHeight = desc->BufferDesc.Height;
  if (!clientWidth || !clientHeight)
    return;

  RECT client = {};
  if (GetClientRect(desc->OutputWindow, &client) &&
      client.right - client.left == LONG(clientWidth) &&
      client.bottom - client.top == LONG(clientHeight))
    return;

  // The window has to end up with this CLIENT area, so the frame is measured
  // rather than assumed: a border and caption are not the same width on every
  // theme, and getting it wrong here would trade one wrong window size for
  // another.
  RECT want = { 0, 0, LONG(clientWidth), LONG(clientHeight) };
  const LONG style = GetWindowLongA(desc->OutputWindow, GWL_STYLE);
  const LONG exStyle = GetWindowLongA(desc->OutputWindow, GWL_EXSTYLE);
  if (!AdjustWindowRectEx(&want, DWORD(style), FALSE, DWORD(exStyle)))
    return;

  // Size only. The game placed the window where it wanted it, and moving it is
  // not this fix's business.
  SetWindowPos(desc->OutputWindow, nullptr, 0, 0, want.right - want.left,
               want.bottom - want.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed))
    log("SSAA present clamp: output window resized to a ", std::dec,
        clientWidth, "x", clientHeight, " client area (the engine had sized it"
        " from the render resolution in its own ini)");
}

// The composite resamples down to the swap-chain size, because the clamp forced
// the back buffer to it. The main render size is never learned on this engine --
// the high-resolution fix is unsupported here -- so asking for it would decline
// every call.
bool substitutionDestSize(unsigned int* width, unsigned int* height) {
  return highResSwapChainSize(width, height);
}

// The clamp resized the back buffer behind the engine's back, so the ini's
// display size is the authority and the target's own size is not.
bool compositeViewportSize(ID3D11DeviceContext*, unsigned int* width,
                           unsigned int* height) {
  return displaySize(width, height);
}

}  // namespace

bool ktglPresentClampEnabled() {
  static const bool on = [] {
    // The env switch forces it on for an experiment on any engine.
    if (const char* env = std::getenv("DUSK_PRESENT_CLAMP"))
      return env[0] != '0';
    // Otherwise it is simply what supersampling means on KTGL. The two engines
    // reach the same feature by opposite routes and the difference is not a
    // preference: Ayesha pins its scene targets, so the mod has to enlarge them
    // and own the resolve; KTGL sizes everything from its own ini, so the mod
    // only has to stop the swap chain following it up. Choosing by engine rather
    // than by a key keeps that from being something a user can get wrong.
    //
    // THE ENGINE TEST IS LOAD-BEARING and was briefly dropped when this moved
    // out of core, on the reasoning that the dispatcher in engine.cpp only ever
    // hands the KTGL policy to a KTGL process. That is true of the policy and
    // false of this function: window_size.cpp calls it directly, from DllMain,
    // in every process. Without the test it reduced to ssaaConfigured(), which
    // is true on Ayesha at any factor above 100 -- so Ayesha installed KTGL's
    // window hooks and resolved a display size it has no use for.
    return currentEngine() == Engine::Ktgl && ssaaConfigured();
  }();
  return on;
}

bool ktglClampedDisplaySize(unsigned int* width, unsigned int* height) {
  if (!ktglPresentClampEnabled())
    return false;
  return displaySize(width, height);
}

bool ktglClampedRenderSize(unsigned int* width, unsigned int* height) {
  const UINT w = g_clampedRenderWidth.load(std::memory_order_relaxed);
  const UINT h = g_clampedRenderHeight.load(std::memory_order_relaxed);
  if (!w || !h)
    return false;
  *width = w;
  *height = h;
  return true;
}

const SsaaPolicy& ktglSsaaPolicy() {
  static const SsaaPolicy policy = {
    ktglPresentClampEnabled,
    substitutionDestSize,
    compositeViewportSize,
    clampPresentSize,
    fitOutputWindow,
    true,    // clamps, so the DXGI resize hooks are installed
    false,   // no scene test on this engine; the host is recognised by shape
    false,   // the engine enlarges from its own ini, not from the high-res fix
  };
  return policy;
}

}  // namespace atfix
