// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// window_background.h; what is here is the per-build wiring and the notes that
// only mean anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "game.h"       // atfix::currentTitle / Title
#include "hook_util.h"  // atfix::installMinHookDetour
#include "log.h"
#include "window_background.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using PFN_RegisterClassExA = ATOM (WINAPI*)(const WNDCLASSEXA*);

PFN_RegisterClassExA originalRegisterClassExA = nullptr;

// The engine window classes and the stock brush each one paints before the
// first frame arrives.
//
// TWO ROWS, because the trilogy has two engines and they do not agree on
// either half. Ayesha registers "KTGL.A11" -- the string it shares with all six
// Arland executables -- with the grey stock brush. Escha & Logy and Shallie
// register "ElixirFramework" with the WHITE stock brush, which is why Shallie
// started on a white flash for as long as this file only knew Ayesha's row.
//
// The brush is part of the row rather than a separate test, so a class is only
// touched when it is painting the exact colour this fix exists to replace. Read
// from the game at the moment it registered the class; two rounds of guessing
// the name out of strings in the image found the wrong candidate in one game
// and nothing in the other.
struct EngineClass {
  const char* name;
  int stockBrush;
};
constexpr EngineClass kEngineClasses[] = {
  { "KTGL.A11",        GRAY_BRUSH },   // Ayesha, and the Arland six
  { "ElixirFramework", WHITE_BRUSH },  // Escha & Logy, Shallie
};

// The row this class matches, or null. Both halves have to agree.
const EngineClass* engineClassFor(const WNDCLASSEXA* wc) {
  // A class name can be an atom rather than a pointer; those are never ours.
  if (!wc->lpszClassName || IS_INTRESOURCE(wc->lpszClassName))
    return nullptr;
  for (const EngineClass& row : kEngineClasses)
    if (!lstrcmpA(wc->lpszClassName, row.name) &&
        wc->hbrBackground == static_cast<HBRUSH>(GetStockObject(row.stockBrush)))
      return &row;
  return nullptr;
}

ATOM WINAPI hookedRegisterClassExA(const WNDCLASSEXA* wc) {
  if (!wc)
    return originalRegisterClassExA(wc);

  // Two conditions have to hold together: the class comes from the executable
  // itself rather than from an injected DLL, and it matches one of the rows
  // above on BOTH its name and its background brush. Stock-object handles are
  // process-wide constants, so that comparison is exact. Anything else
  // registers unchanged, which also means this quietly stands down if a build
  // ever stops doing it.
  if (wc->hInstance != GetModuleHandleW(nullptr) || !engineClassFor(wc))
    return originalRegisterClassExA(wc);

  WNDCLASSEXA substitute = *wc;
  substitute.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  const ATOM atom = originalRegisterClassExA(&substitute);
  log("FIXES window_background=black class=", wc->lpszClassName,
    " atom=", static_cast<unsigned>(atom));
  return atom;
}

}  // namespace

void installWindowBackgroundFix() {
  static bool attempted = false;
  if (attempted)
    return;
  attempted = true;

  if (currentTitle() == Title::Unknown)
    return;

  // user32 is a static import of this DLL, so the loader has mapped it before
  // this runs and the lookup cannot fail. No LoadLibrary fallback: this is
  // called from DllMain, where LoadLibrary is forbidden.
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) {
    log("Window-background fix: user32.dll unavailable");
    return;
  }

  auto* registerClassExA = reinterpret_cast<BYTE*>(
    GetProcAddress(user32, "RegisterClassExA"));
  if (!registerClassExA)
    return;

  // Nothing is logged here on success. The class has not been registered yet,
  // so the interesting line is the one the hook writes when it substitutes.
  installMinHookDetour(registerClassExA,
    reinterpret_cast<void*>(&hookedRegisterClassExA),
    reinterpret_cast<void**>(&originalRegisterClassExA));
}

}  // namespace atfix
