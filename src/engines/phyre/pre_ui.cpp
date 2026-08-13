// SPDX-License-Identifier: MIT
//
// See pre_ui.h for why the scene target is identified by what is drawn into it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <new>

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

// The target and draw run belong to the context that recorded them. Keeping
// them in one process-global pair let an immediate/deferred interleave attach
// one context's draw count to another context's surface; the pointer's
// load-then-AddRef sequence could also race its replacement. A context-owned
// COM object keeps the tuple together and retains the surface for precisely the
// context lifetime that can consume it.
const GUID IID_DuskPhyrePreUiState =
  { 0xec81b85e, 0x73bc, 0x44f8,
    { 0x8a, 0xd4, 0x2d, 0x3c, 0x66, 0x30, 0x0f, 0x5e } };

struct PreUiContextState final : IUnknown {
  std::atomic<ULONG> references{1};
  uint64_t frame = 0;
  uint32_t draws = 0;
  ID3D11Texture2D* current = nullptr;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown))
      return E_NOINTERFACE;
    *out = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return references.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = references.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (!left)
      delete this;
    return left;
  }

  void setCurrent(ID3D11Texture2D* next) {
    if (next)
      next->AddRef();
    if (current)
      current->Release();
    current = next;
    draws = 0;
  }

private:
  ~PreUiContextState() { setCurrent(nullptr); }
};

std::atomic<uint64_t> g_preUiFrame{1};
std::atomic<uint64_t> g_claimedFrame{0};

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

PreUiContextState* preUiState(ID3D11DeviceContext* context) {
  IUnknown* stored = nullptr;
  UINT size = sizeof(stored);
  if (SUCCEEDED(context->GetPrivateData(IID_DuskPhyrePreUiState,
                                        &size, &stored)) && stored)
    return static_cast<PreUiContextState*>(stored);  // GetPrivateData AddRef'd

  auto* created = new (std::nothrow) PreUiContextState;
  if (!created)
    return nullptr;
  if (FAILED(context->SetPrivateDataInterface(IID_DuskPhyrePreUiState,
                                               created))) {
    created->Release();
    return nullptr;
  }
  return created;
}

void enterCurrentFrame(PreUiContextState* state) {
  const uint64_t frame = g_preUiFrame.load(std::memory_order_acquire);
  if (state->frame == frame)
    return;
  state->setCurrent(nullptr);
  state->frame = frame;
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
  if (ssaaEngaged()) {
    // Drop any target retained before supersampling first engaged. This path
    // no longer consumes it, so keeping it to context destruction would turn a
    // mode transition into a session-long resource retention.
    context->SetPrivateDataInterface(IID_DuskPhyrePreUiState, nullptr);
    return;
  }

  PreUiContextState* state = preUiState(context);
  if (!state)
    return;
  enterCurrentFrame(state);

  ID3D11Texture2D* arriving =
    (numViews && views) ? textureOf(views[0]) : nullptr;
  ID3D11Texture2D* leaving = state->current;
  if (leaving)
    leaving->AddRef();

  // Still the same surface: the pass that is running has not finished.
  if (arriving && arriving == leaving) {
    arriving->Release();
    leaving->Release();
    state->Release();
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
  const uint32_t draws = state->draws;
  const uint64_t frame = g_preUiFrame.load(std::memory_order_acquire);
  uint64_t claimed = g_claimedFrame.load(std::memory_order_relaxed);
  if (leaving && draws >= kSceneDrawThreshold && claimed != frame &&
      g_claimedFrame.compare_exchange_strong(
        claimed, frame, std::memory_order_acq_rel,
        std::memory_order_relaxed)) {
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
  }

  state->setCurrent(arriving);
  if (arriving)
    arriving->Release();   // the state took its own reference
  if (leaving)
    leaving->Release();
  state->Release();
}

void phyrePreUiAfterDraw(ID3D11DeviceContext* context) {
  if (t_inPass || !context)
    return;
  if (!smaaPreUiEnabled() && !sharpenEnabled())
    return;
  PreUiContextState* state = preUiState(context);
  if (!state)
    return;
  enterCurrentFrame(state);
  ++state->draws;
  state->Release();
}

void phyrePreUiFrameTick() {
  // The bound target does NOT carry over. Contexts cannot be enumerated here,
  // so advancing an epoch makes each context release its own target and reset
  // its count lazily on the next bind/draw.
  g_preUiFrame.fetch_add(1, std::memory_order_release);
}

}  // namespace atfix
