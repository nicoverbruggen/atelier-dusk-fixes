// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// letterbox.h; what is here is the arithmetic and the notes that only mean
// anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "letterbox.h"
#include "game.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The tag. Private data on the texture itself, so the identity survives every
// view the engine makes over it and costs one query at a bind.
const GUID IID_DuskLetterboxBack =
  {0x9d2c4f71,0x6a18,0x4b92,{0xa4,0x53,0x1c,0x87,0x2e,0x60,0xd9,0x14}};

std::atomic<UINT> g_backWidth{0};
std::atomic<UINT> g_backHeight{0};
std::atomic<UINT> g_fittedWidth{0};
std::atomic<UINT> g_fittedHeight{0};
std::atomic<bool> g_active{false};

// The largest 16:9 rectangle inside this one, on whichever axis is already the
// constraint. Even dimensions, because an odd size centres on a half-pixel and
// leaves a seam down one edge of the picture rather than in the bars.
void fitToSixteenNine(UINT width, UINT height, UINT* outWidth,
                      UINT* outHeight) {
  *outWidth = width;
  *outHeight = height;
  const uint64_t wide = uint64_t(width) * 9;
  const uint64_t tall = uint64_t(height) * 16;
  if (wide == tall)
    return;
  if (wide > tall)
    *outWidth = UINT(uint64_t(height) * 16 / 9);    // wider: bars left and right
  else
    *outHeight = UINT(uint64_t(width) * 9 / 16);    // taller: bars top and bottom
  *outWidth &= ~1u;
  *outHeight &= ~1u;
}

}  // namespace

bool letterboxEnabled();

void letterboxNoteSwapChain(IDXGISwapChain* swapChain) {
  if (!swapChain)
    return;
  // Windowed or not, and how big the window is, reported before any decision.
  // A window is not the same problem as a screen: a screen has to be filled, so
  // a 16:9 picture on a 4:3 one needs bars, while a window can simply be 16:9
  // and then there is nothing to put bars in. The Arland project needed exactly
  // this pair of numbers to tell the two cases apart, so they are gathered here
  // before the same question has to be answered on this engine.
  DXGI_SWAP_CHAIN_DESC chain = { };
  if (SUCCEEDED(swapChain->GetDesc(&chain))) {
    RECT client = { };
    const bool haveClient = chain.OutputWindow &&
                            GetClientRect(chain.OutputWindow, &client);
    log("LETTERBOX swap chain ", std::dec, chain.BufferDesc.Width, "x",
        chain.BufferDesc.Height,
        chain.Windowed ? " windowed" : " fullscreen",
        ", output window client area ",
        haveClient ? long(client.right - client.left) : 0L, "x",
        haveClient ? long(client.bottom - client.top) : 0L);
  }

  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_ID3D11Texture2D,
                                  reinterpret_cast<void**>(&back))) || !back)
    return;
  D3D11_TEXTURE2D_DESC desc = {};
  back->GetDesc(&desc);
  const UINT marker = 1;
  back->SetPrivateData(IID_DuskLetterboxBack, sizeof(marker), &marker);
  back->Release();

  const UINT width = desc.Width;
  const UINT height = desc.Height;
  if (!width || !height)
    return;
  if (g_backWidth.load(std::memory_order_relaxed) == width &&
      g_backHeight.load(std::memory_order_relaxed) == height)
    return;

  UINT fittedWidth = 0, fittedHeight = 0;
  fitToSixteenNine(width, height, &fittedWidth, &fittedHeight);

  // A panel that is ALMOST 16:9 is left alone. 1366x768 is off by 0.05%, so the
  // correction takes two pixels off the width and the whole reward is a
  // two-pixel black bar down each side.
  const UINT lostWidth = width - fittedWidth;
  const UINT lostHeight = height - fittedHeight;
  const bool worthIt = uint64_t(lostWidth) * 100 >= width ||
                       uint64_t(lostHeight) * 100 >= height;

  g_backWidth.store(width, std::memory_order_relaxed);
  g_backHeight.store(height, std::memory_order_relaxed);
  g_fittedWidth.store(fittedWidth, std::memory_order_relaxed);
  g_fittedHeight.store(fittedHeight, std::memory_order_relaxed);
  g_active.store(worthIt, std::memory_order_release);

  if (worthIt && featureSupport(Feature::Letterbox) == Support::Unsupported)
    log("FIXES letterbox=not_applicable (this engine fits a 16:9 render into"
        " the back buffer itself and the launcher writes it a 16:9 render size,"
        " so this ", std::dec, width, "x", height, " back buffer is left alone;"
        " fitting it here as well is what makes the picture too small)");
  else if (worthIt && !letterboxEnabled())
    log("FIXES letterbox=off (DUSK_LETTERBOX=0); this ", std::dec, width, "x",
        height, " back buffer would have been fitted to ", fittedWidth, "x",
        fittedHeight, ", so whatever shape the frame comes out is the engine's"
        " own doing");
  else if (worthIt)
    log("FIXES letterbox=active ", std::dec, fittedWidth, "x", fittedHeight,
        " inside a ", width, "x", height, " back buffer (",
        lostHeight ? "bars top and bottom" : "bars left and right", ")");
  else
    log("FIXES letterbox=not_needed (the back buffer is ", std::dec, width,
        "x", height, ", which is 16:9 or within one percent of it)");
}

LetterboxFitPass g_fitPass = nullptr;
void (*g_fitPreload)() = nullptr;

void letterboxSetFitPass(LetterboxFitPass pass, void (*preload)()) {
  g_fitPass = pass;
  g_fitPreload = preload;
}

void letterboxPreload() {
  if (g_fitPreload && letterboxActive())
    g_fitPreload();
}

bool letterboxApply(IDXGISwapChain* swapChain) {
  if (!g_fitPass || !letterboxActive())
    return false;
  return g_fitPass(swapChain);
}

// Whether this game wants the pass at all, from the capability matrix, with
// DUSK_LETTERBOX=0 standing it down for a session.
//
// THE SWITCH EARNED ITS PLACE ON AYESHA. An engine that already fits its own
// frame and one that stretches it both look wrong once a second fit lands on
// top, and by eye the two are nearly the same complaint: a picture that is too
// small with too much black around it. Turning the pass off is what separates
// them, and on Ayesha the frame came out correctly proportioned without it,
// which is why that row is Unsupported.
bool letterboxEnabled() {
  // Resolved once. letterboxPreload and letterboxApply both reach this from
  // the hooked Present, so an uncached read is two trips through
  // featureEnabled() per frame -- a getenv and a GetPrivateProfileString that
  // also seeds the key when it is absent. The rule that hooks on the render
  // thread touch neither is stated in engines/phyre/logo_skip.cpp.
  static const bool enabled = featureEnabled(Feature::Letterbox);
  return enabled;
}

bool letterboxActive() {
  return letterboxEnabled() && g_active.load(std::memory_order_acquire);
}

bool letterboxViewportFor(UINT targetWidth, UINT targetHeight,
                          D3D11_VIEWPORT* viewport) {
  if (!viewport || !letterboxActive())
    return false;
  // Only the back buffer. Every other target in the frame is the engine's own
  // and is already whatever shape it means to be; narrowing one of those would
  // move the picture rather than frame it.
  if (targetWidth != g_backWidth.load(std::memory_order_relaxed) ||
      targetHeight != g_backHeight.load(std::memory_order_relaxed))
    return false;

  const float fittedWidth = float(g_fittedWidth.load(std::memory_order_relaxed));
  const float fittedHeight = float(g_fittedHeight.load(std::memory_order_relaxed));
  viewport->TopLeftX = (float(targetWidth) - fittedWidth) * 0.5f;
  viewport->TopLeftY = (float(targetHeight) - fittedHeight) * 0.5f;
  viewport->Width = fittedWidth;
  viewport->Height = fittedHeight;
  viewport->MinDepth = 0.0f;
  viewport->MaxDepth = 1.0f;
  return true;
}

}  // namespace atfix
