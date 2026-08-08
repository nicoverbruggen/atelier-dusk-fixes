// SPDX-License-Identifier: MIT
//
// See pre_ui.h for why the scene target is identified by what is drawn into it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>

#include "pre_ui.h"
#include "../../core/log.h"
#include "../../core/sharpen.h"
#include "../../core/smaa.h"
#include "../../core/supersample.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// Enough draws to be the 3D pass rather than a stray depth-tested blit. The
// number is Arland's, which has run in three shipped games: the post-processing
// chain issues a handful of full-screen draws per surface and the 3D pass
// issues hundreds, so anything between the two separates them and there is no
// value in tuning it finer.
constexpr uint32_t kSceneDrawThreshold = 24;

// The target currently bound, and how many draws have gone into it since it
// was. A reference is held, because the pass runs at the bind that replaces it
// and nothing else guarantees the surface is still alive then.
std::atomic<ID3D11Texture2D*> g_current{nullptr};
std::atomic<uint32_t> g_currentDraws{0};

// One firing per frame. Reset at Present.
std::atomic<bool> g_firedThisFrame{false};

// Re-entry guard. The passes bind render targets of their own, so they come
// straight back through the bind that triggered them; without this the anchor
// would fire inside its own pass. Thread-local because the recursion is always
// on the recording thread, and a global would let one recording thread suppress
// another's legitimate firing.
thread_local bool t_inPass = false;

// The texture behind a view, or null. The caller owns the reference.
ID3D11Texture2D* textureOf(ID3D11RenderTargetView* view) {
  if (!view)
    return nullptr;
  ID3D11Resource* resource = nullptr;
  view->GetResource(&resource);
  if (!resource)
    return nullptr;
  ID3D11Texture2D* texture = nullptr;
  resource->QueryInterface(IID_ID3D11Texture2D,
                           reinterpret_cast<void**>(&texture));
  resource->Release();
  return texture;
}

void setCurrent(ID3D11Texture2D* texture) {
  if (texture)
    texture->AddRef();
  if (ID3D11Texture2D* previous =
        g_current.exchange(texture, std::memory_order_relaxed))
    previous->Release();
  g_currentDraws.store(0, std::memory_order_relaxed);
}

}  // namespace

void phyrePreUiNoteTargets(ID3D11DeviceContext* context, unsigned int numViews,
                           ID3D11RenderTargetView* const* views) {
  if (t_inPass || !context)
    return;
  if (!smaaPreUiEnabled() && !sharpenEnabled())
    return;
  // Not when supersampling has engaged: the downscale runs both passes at
  // display resolution and still before the interface. Two pre-UI passes do not
  // stack, they alternate -- they share one per-frame latch. See the
  // smaaApplySceneColor call in supersample.cpp.
  if (ssaaEngaged())
    return;

  ID3D11Texture2D* arriving =
    (numViews && views) ? textureOf(views[0]) : nullptr;
  ID3D11Texture2D* leaving = g_current.load(std::memory_order_relaxed);

  // Still the same surface: the pass that is running has not finished.
  if (arriving && arriving == leaving) {
    arriving->Release();
    return;
  }

  // The surface being left is the scene if the frame's 3D pass went into it.
  //
  // THIS IS THE WHOLE POINT. The test that used to decide this was the
  // size-and-format rule in scene_target.cpp, and that rule matches BOTH of the
  // colour targets this engine ping-pongs between through its post chain --
  // correctly, they are both scene targets. What it cannot say is which one the
  // player is about to be shown, so the pass fired on the first of the pair to
  // be released, somewhere in the middle of the chain, and claimed the frame's
  // one SMAA run doing it. A draw count separates them because only one of the
  // two receives the 3D pass.
  const uint32_t draws = g_currentDraws.load(std::memory_order_relaxed);
  if (leaving && draws >= kSceneDrawThreshold &&
      !g_firedThisFrame.exchange(true, std::memory_order_relaxed)) {
    leaving->AddRef();   // held across the passes, which replace g_current
    t_inPass = true;
    // Order is a correctness requirement, not a preference: sharpening first
    // would hand SMAA harder edges to find and it would blend them away again.
    // See sharpen.h.
    const bool smoothed = smaaApplySceneColor(context, leaving);
    const bool sharpened = sharpenApply(context, leaving);
    t_inPass = false;

    // Said once, and only on a real success. A line printed before the calls
    // reported the previous anchor working for every session it did not.
    static std::atomic<bool> announced{false};
    if ((smoothed || sharpened) &&
        !announced.exchange(true, std::memory_order_relaxed)) {
      D3D11_TEXTURE2D_DESC d = {};
      leaving->GetDesc(&d);
      log("Pre-UI: active on ", std::dec, d.Width, "x", d.Height,
          " after ", draws, " draws -- smoothing=", smoothed ? "on" : "off",
          " sharpening=", sharpened ? "on" : "off",
          ", on the surface the frame's 3D pass was drawn into, before the"
          " interface");
    }
    leaving->Release();
  }

  setCurrent(arriving);
  if (arriving)
    arriving->Release();   // setCurrent took its own reference
}

void phyrePreUiAfterDraw(ID3D11DeviceContext*) {
  if (t_inPass)
    return;
  if (!smaaPreUiEnabled() && !sharpenEnabled())
    return;
  g_currentDraws.fetch_add(1, std::memory_order_relaxed);
}

void phyrePreUiFrameTick() {
  g_firedThisFrame.store(false, std::memory_order_relaxed);
  // The bound target does NOT carry over: a frame starts by binding its own,
  // and a count left standing would credit the next frame's first surface with
  // the previous one's draws.
  setCurrent(nullptr);
}

}  // namespace atfix
