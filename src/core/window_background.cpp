// SPDX-License-Identifier: MIT
//
// Grey-flash fix, ported from the Arland project's src/window_background.cpp
// (this project's own code, MIT). Nothing in it needed changing, and that is
// worth stating: Ayesha registers the same window class under the same name
// with the same brush, so the Arland file works here as written.
//
// Ayesha registers its window class with GRAY_BRUSH as the class background.
// Both builds, at the single reference to the class-name string:
//
//     lea  rax, [rip + …]                ; "KTGL.A11"
//     mov  qword ptr [rbp - 0x40], rax   ; WNDCLASSEXA::lpszClassName
//     mov  edx, 0x7f00                   ; IDC_ARROW
//     xor  ecx, ecx
//     call LoadCursorA
//     mov  qword ptr [rbp - 0x58], rax   ; WNDCLASSEXA::hCursor
//     lea  ecx, [r13 + 2]                ; 2 = GRAY_BRUSH
//     call GetStockObject
//     mov  qword ptr [rbp - 0x50], rax   ; WNDCLASSEXA::hbrBackground
//
// (EN 0x921d5f, ML 0x94425f, both inside the single function that references
// the string.) That is instruction-for-instruction what the six Arland
// executables do, down to the frame offsets.
//
// The window procedure is short and forwards everything it does not special
// case to DefWindowProcA, so WM_ERASEBKGND is answered by filling the client
// area with that brush. Between the window appearing and the first Present
// there is therefore a mid-grey (128,128,128) rectangle, which is the grey
// screen at startup. It lasts as long as device creation and the first frame's
// work, and it is the game's own doing rather than anything Wine or Proton
// adds; Windows shows it too.
//
// Black is what the game fades up from, so the flash disappears into the intro
// instead of announcing itself. Substituting the brush at registration is
// enough: nothing else reads hbrBackground, and the class is registered once.
//
// This hooks RegisterClassExA rather than patching the call site because the
// substitution needs no addresses and no prologue gating. It runs from
// DLL_PROCESS_ATTACH, before the game's entry point, so it is in place by the
// time the class is registered.
//
// ESCHA & LOGY AND SHALLIE ARE NOT COVERED, and this file needs no per-game
// gate to express that. Neither executable contains the string "KTGL.A11" at
// all, so the name test below simply never matches there. If their own class
// name is ever established, adding it to the test is the whole change.
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
// from the game at the moment it registers the class, with
// DUSK_WINDOW_CLASS_TRACE=1; two rounds of guessing the name out of strings in
// the image found the wrong candidate in one game and nothing in the other.
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

// `DUSK_WINDOW_CLASS_TRACE=1` names every class the executable registers, with
// the stock brush its background is, if any.
//
// WHY IT EXISTS. The substitution below is keyed on a class name read out of
// Ayesha's executable, and Shallie starts on a WHITE screen -- so its class is
// named something else, or its brush is a different stock object, or both. The
// three games all import RegisterClassExA, so the hook is live in all of them
// and the rule is simply declining. Guessing a second name from strings in the
// image found `A11R` in Escha and nothing at all in Shallie, which is exactly
// the kind of resemblance this project has been caught by before. The game
// states its own class name at the moment it registers it; this reads it there.
bool classTraceEnabled() {
  const char* value = std::getenv("DUSK_WINDOW_CLASS_TRACE");
  return value && value[0] != '0';
}

const char* stockBrushName(HBRUSH brush) {
  if (!brush) return "none";
  struct Entry { int id; const char* name; };
  static const Entry kStock[] = {
    { WHITE_BRUSH, "WHITE_BRUSH" }, { LTGRAY_BRUSH, "LTGRAY_BRUSH" },
    { GRAY_BRUSH, "GRAY_BRUSH" },   { DKGRAY_BRUSH, "DKGRAY_BRUSH" },
    { BLACK_BRUSH, "BLACK_BRUSH" }, { NULL_BRUSH, "NULL_BRUSH" },
  };
  for (const Entry& e : kStock)
    if (brush == static_cast<HBRUSH>(GetStockObject(e.id)))
      return e.name;
  // A system-colour brush is passed as COLOR_x + 1 rather than a handle, which
  // is why this is worth telling apart from a real brush.
  if (reinterpret_cast<uintptr_t>(brush) <= 32)
    return "system colour (COLOR_x + 1)";
  return "a brush of its own";
}

ATOM WINAPI hookedRegisterClassExA(const WNDCLASSEXA* wc) {
  if (!wc)
    return originalRegisterClassExA(wc);

  if (classTraceEnabled() && wc->hInstance == GetModuleHandleW(nullptr))
    log("WINCLASS name=\"",
        (wc->lpszClassName && !IS_INTRESOURCE(wc->lpszClassName))
          ? wc->lpszClassName : "(atom)",
        "\" background=", stockBrushName(wc->hbrBackground),
        " style=0x", std::hex, wc->style, std::dec);

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
