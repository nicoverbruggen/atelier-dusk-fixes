// SPDX-License-Identifier: MIT
//
// Right-sizing the game window for Escha & Logy and Shallie, at creation.
//
// WHY THE WINDOW IS WRONG AT ALL. Supersampling on these two works by writing
// the game's own Setting.ini at base x factor, so the engine renders everything
// larger, and then clamping the swap chain back down to the base. The engine
// also sizes its WINDOW from that ini, so at 150% on a 1920x1080 base it asks
// for a 2880x1620 window to hold a 1920x1080 image. Fullscreen hides this
// because the display mode decides the size instead.
//
// WHY AT CREATION RATHER THAN AFTER. Correcting it once the window exists works,
// and that is what shipped first, but the player sees the wrong window and then
// watches it change -- more than once, because more than one thing along the
// startup path sets a size. Sizing the CreateWindowEx call means the window is
// never wrong, so there is nothing to correct and nothing to see.
//
// WHAT IS SUBSTITUTED. The size handed to CreateWindowEx is the WINDOW rect,
// frame included, while the size that has to come out right is the CLIENT area.
// The frame is measured with AdjustWindowRectEx from the styles of the call
// being made, not assumed: a caption and border are not the same thickness on
// every theme or under every compositor, and guessing here would trade one
// wrong size for another.
//
// HOW THE WINDOW IS IDENTIFIED. Three things together, because none alone is
// enough. The call must come from the game's own module; the clamp route must be
// active, which only happens on these two games with supersampling on; and the
// requested client area must actually be the render size the mod is clamping
// away. A window the game opens for anything else does not match the third and
// is left alone.
//
// The post-creation fit in core stays as it is. It costs nothing once this hook
// has done its job, since it returns immediately when the client area is already
// right, and it is what covers a window this hook did not see.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdlib>

#include "window_size.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/supersample.h"

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
      ssaaClampedDisplaySize(&displayWidth, &displayHeight)) {
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

// ---- DUSK_WINDOW_TRACE ------------------------------------------------------
//
// Where does the game's window actually get its size? The creation hook above
// never saw it: the only CreateWindowExA it caught passed CW_USEDEFAULT, while
// the window that reached the swap chain was 2562x1416. So the size is set
// somewhere else, and rather than guess between the candidates a third time,
// this records all of them. Bounded, because SetWindowPos is called freely.

using PFN_CreateWindowExW = HWND (WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                           int, int, int, HWND, HMENU,
                                           HINSTANCE, LPVOID);
using PFN_SetWindowPos = BOOL (WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using PFN_MoveWindow = BOOL (WINAPI*)(HWND, int, int, int, int, BOOL);

PFN_CreateWindowExW originalCreateWindowExW = nullptr;
PFN_SetWindowPos originalSetWindowPos = nullptr;
PFN_MoveWindow originalMoveWindow = nullptr;

std::atomic<bool> tracing{false};
std::atomic<int> traceLines{0};
constexpr int kTraceLimit = 24;

bool traceRoom() {
  if (!tracing.load(std::memory_order_relaxed))
    return false;
  const int n = traceLines.fetch_add(1, std::memory_order_relaxed);
  if (n == kTraceLimit)
    log("WINDOWTRACE: ", std::dec, kTraceLimit,
        " lines logged; further window calls are not listed");
  return n < kTraceLimit;
}

HWND WINAPI tracedCreateWindowExW(DWORD exStyle, LPCWSTR className,
                                  LPCWSTR windowName, DWORD style, int x, int y,
                                  int width, int height, HWND parent,
                                  HMENU menu, HINSTANCE instance,
                                  LPVOID param) {
  if (traceRoom())
    log("WINDOWTRACE: CreateWindowExW ", std::dec, width, "x", height,
        " at ", x, ",", y, instance == GetModuleHandleW(nullptr)
          ? " (game module)" : " (other module)");
  return originalCreateWindowExW(exStyle, className, windowName, style, x, y,
                                 width, height, parent, menu, instance, param);
}

BOOL WINAPI tracedSetWindowPos(HWND hwnd, HWND after, int x, int y, int cx,
                               int cy, UINT flags) {
  if (!(flags & SWP_NOSIZE) && traceRoom())
    log("WINDOWTRACE: SetWindowPos ", std::dec, cx, "x", cy, " at ", x, ",", y,
        (flags & SWP_NOMOVE) ? " (size only)" : "");

  // THIS is where the window gets its size, not CreateWindowEx. A trace on
  // 2026-08-09 showed the window created 1x1 by another module and then sized
  // here to 2890x1656 -- a 2880x1620 client plus frame -- before the game ever
  // touches D3D11. Correcting it at creation could not work because at creation
  // there was no size to correct.
  if (sizing.load(std::memory_order_relaxed) && !(flags & SWP_NOSIZE) &&
      cx > 0 && cy > 0) {
    unsigned int displayWidth = 0, displayHeight = 0;
    if (ssaaClampedDisplaySize(&displayWidth, &displayHeight)) {
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

BOOL WINAPI tracedMoveWindow(HWND hwnd, int x, int y, int width, int height,
                             BOOL repaint) {
  if (traceRoom())
    log("WINDOWTRACE: MoveWindow ", std::dec, width, "x", height, " at ", x,
        ",", y);
  return originalMoveWindow(hwnd, x, y, width, height, repaint);
}

void installWindowTrace(HMODULE user32) {
  // The SetWindowPos detour is the FIX, so it goes in whether or not the trace
  // is on; DUSK_WINDOW_TRACE only decides whether these three also narrate.
  const char* on = std::getenv("DUSK_WINDOW_TRACE");
  tracing.store(on && on[0] != '0', std::memory_order_relaxed);
  struct Hook { const char* name; void* replacement; void** original; };
  const Hook kHooks[] = {
    { "CreateWindowExW", reinterpret_cast<void*>(&tracedCreateWindowExW),
      reinterpret_cast<void**>(&originalCreateWindowExW) },
    { "SetWindowPos", reinterpret_cast<void*>(&tracedSetWindowPos),
      reinterpret_cast<void**>(&originalSetWindowPos) },
    { "MoveWindow", reinterpret_cast<void*>(&tracedMoveWindow),
      reinterpret_cast<void**>(&originalMoveWindow) },
  };
  int live = 0;
  for (const Hook& hook : kHooks) {
    void* target = reinterpret_cast<void*>(GetProcAddress(user32, hook.name));
    if (target && installMinHookDetour(static_cast<BYTE*>(target),
                                       hook.replacement, hook.original))
      ++live;
    else
      log("WINDOWTRACE: could not hook ", hook.name);
  }
  if (tracing.load(std::memory_order_relaxed))
    log("WINDOWTRACE: active, ", std::dec, live,
        " of 3 entry points (nothing is changed)");
}

}  // namespace

bool installKtglWindowSize() {
  // INSTALLED FROM DllMain, because the game creates its window before it
  // touches D3D11. Installing this from the engine module's own initialization
  // -- which runs when the D3D11 proxy is first used -- was too late by the
  // time the window already existed, and left the after-the-fact resize as the
  // only thing doing the work, which is the flashing this replaces.
  if (!ssaaPresentClampEnabled())
    return false;

  HMODULE user32 = GetModuleHandleA("user32.dll");
  if (!user32)
    return false;
  installWindowTrace(user32);
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
