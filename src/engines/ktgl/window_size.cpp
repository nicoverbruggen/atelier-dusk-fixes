// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// window_size.h; what is here is the per-build wiring and the notes that
// only mean anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

#include "window_size.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/supersample.h"
#include "present_clamp.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using PFN_CreateWindowExA = HWND (WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int,
                                           int, int, int, HWND, HMENU, HINSTANCE,
                                           LPVOID);

PFN_CreateWindowExA originalCreateWindowExA = nullptr;
std::atomic<bool> sizing{false};

HWND WINAPI hookedCreateWindowExA(DWORD exStyle, LPCSTR className,
                                  LPCSTR windowName, DWORD style, int x, int y,
                                  int width, int height, HWND parent,
                                  HMENU menu, HINSTANCE instance,
                                  LPVOID param) {
  int useWidth = width;
  int useHeight = height;

  // CW_USEDEFAULT is 0x80000000, and subtracting a frame from it wraps through
  // signed overflow into a huge positive number that passes every size test
  // below. An observed run resized the wrong window because of exactly that:
  // the log reported the caller asking for 2147483644x2147483644. A window
  // whose size the caller has not stated is not one this hook can reason about.
  const bool sizeStated = width > 0 && height > 0 &&
                          width != CW_USEDEFAULT && height != CW_USEDEFAULT;

  unsigned int displayWidth = 0, displayHeight = 0;
  if (sizeStated && sizing.load(std::memory_order_relaxed) && !parent &&
      instance == GetModuleHandleW(nullptr) &&
      ktglClampedDisplaySize(&displayWidth, &displayHeight)) {
    // What client area would this call produce as it stands?
    RECT frame = { 0, 0, 0, 0 };
    if (AdjustWindowRectEx(&frame, style, menu != nullptr, exStyle)) {
      const int frameWidth = (frame.right - frame.left);
      const int frameHeight = (frame.bottom - frame.top);
      const int askedClientWidth = width - frameWidth;
      const int askedClientHeight = height - frameHeight;

      // Only the window that is asking for the render size, which is the one
      // the clamp makes wrong. Anything else the game opens is not ours.
      // LARGER THAN, not equal to. The engine asks for its render size, but the
      // window manager may already have clipped that to the desktop before the
      // call is made -- an observed run asked for 2880x1620 and arrived here as
      // 2562x1416. An equality test against the render size matched neither and
      // did nothing at all.
      if (askedClientWidth > int(displayWidth) &&
          askedClientHeight > int(displayHeight)) {
        useWidth = int(displayWidth) + frameWidth;
        useHeight = int(displayHeight) + frameHeight;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
          log("SSAA present clamp: window created at a ", std::dec,
              displayWidth, "x", displayHeight, " client area rather than the ",
              askedClientWidth, "x", askedClientHeight,
              " the engine asked for");
      }
    }
  }

  return originalCreateWindowExA(exStyle, className, windowName, style, x, y,
                                 useWidth, useHeight, parent, menu, instance,
                                 param);
}

using PFN_SetWindowPos = BOOL (WINAPI*)(HWND, HWND, int, int, int, int, UINT);

PFN_SetWindowPos originalSetWindowPos = nullptr;

BOOL WINAPI hookedSetWindowPos(HWND hwnd, HWND after, int x, int y, int cx,
                               int cy, UINT flags) {
  // THIS is where the window gets its size, not CreateWindowEx. A trace
  // showed the window created 1x1 by another module and then sized
  // here to 2890x1656 -- a 2880x1620 client plus frame -- before the game ever
  // touches D3D11. Correcting it at creation could not work because at creation
  // there was no size to correct.
  //
  // THE MODULE TEST IS WHAT NARROWS THIS TO THE GAME'S WINDOW. The comment
  // above records another module creating a window in this process, so "any
  // window bigger than the display on both axes" is not a description of one
  // window. hookedCreateWindowExA already requires the instance to be the
  // executable's own; this is the same test, spelled for a window that exists.
  if (sizing.load(std::memory_order_relaxed) && !(flags & SWP_NOSIZE) &&
      cx > 0 && cy > 0 &&
      reinterpret_cast<HMODULE>(GetWindowLongPtrA(hwnd, GWLP_HINSTANCE)) ==
        GetModuleHandleW(nullptr)) {
    unsigned int displayWidth = 0, displayHeight = 0;
    if (ktglClampedDisplaySize(&displayWidth, &displayHeight)) {
      const LONG style = GetWindowLongA(hwnd, GWL_STYLE);
      const LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
      RECT frame = { 0, 0, 0, 0 };
      if (AdjustWindowRectEx(&frame, DWORD(style), FALSE, DWORD(exStyle))) {
        const int frameWidth = frame.right - frame.left;
        const int frameHeight = frame.bottom - frame.top;
        if (cx - frameWidth > int(displayWidth) &&
            cy - frameHeight > int(displayHeight)) {
          cx = int(displayWidth) + frameWidth;
          cy = int(displayHeight) + frameHeight;
          // AND THE MOVE IS DECLINED, not replaced. The position in this call
          // was computed to place a window of the size being overridden -- an
          // observed run asked for 2890x1656 at 722,414, which for the
          // corrected 1930x1116 puts the window off the bottom right of a
          // 1440p screen. Choosing a different position would be the mod
          // deciding where the game's window belongs, which is not its
          // business; refusing a position derived from a size that no longer
          // applies leaves the window where it already was.
          flags |= SWP_NOMOVE;
          static std::atomic<bool> logged{false};
          if (!logged.exchange(true, std::memory_order_relaxed))
            log("SSAA present clamp: window sized to a ", std::dec,
                displayWidth, "x", displayHeight, " client area at the call"
                " that sets it; that call's position is declined because it was"
                " computed for the larger size");
        }
      }
    }
  }
  return originalSetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

}  // namespace

bool installKtglWindowSize() {
  // INSTALLED FROM DllMain, because the game creates its window before it
  // touches D3D11. Installing this from the engine module's own initialization
  // -- which runs when the D3D11 proxy is first used -- was too late by the
  // time the window already existed, and left the after-the-fact resize as the
  // only thing doing the work, which is the flashing this replaces.
  if (!ktglPresentClampEnabled())
    return false;

  HMODULE user32 = GetModuleHandleA("user32.dll");
  if (!user32)
    return false;

  // The engine creates its window at a placeholder size and assigns the real
  // one here before touching D3D11, so this hook is the primary correction.
  void* setWindowPos = reinterpret_cast<void*>(
    GetProcAddress(user32, "SetWindowPos"));
  if (!setWindowPos ||
      !installMinHookDetour(static_cast<BYTE*>(setWindowPos),
                            reinterpret_cast<void*>(&hookedSetWindowPos),
                            reinterpret_cast<void**>(&originalSetWindowPos))) {
    log("SSAA present clamp: could not hook SetWindowPos; the window is"
        " corrected after creation instead");
  }

  void* target = reinterpret_cast<void*>(
    GetProcAddress(user32, "CreateWindowExA"));
  if (!target) {
    log("SSAA present clamp: CreateWindowExA not found; the window is corrected"
        " after creation instead");
    return false;
  }

  if (!installMinHookDetour(static_cast<BYTE*>(target),
                            reinterpret_cast<void*>(&hookedCreateWindowExA),
                            reinterpret_cast<void**>(&originalCreateWindowExA))) {
    log("SSAA present clamp: could not hook CreateWindowExA; the window is"
        " corrected after creation instead");
    return false;
  }

  sizing.store(true, std::memory_order_relaxed);
  return true;
}

}  // namespace atfix
