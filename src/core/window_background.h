// SPDX-License-Identifier: MIT
#pragma once

// Replaces the window class's grey background brush with black, so the startup
// flash before the first frame is black rather than mid-grey.
//
// Must run before the game registers its window class, which means DllMain.
// Idempotent, and it installs nothing when the running executable is not one of
// the three games.
//
// Grey-flash fix, ported from the Arland project's src/core/window_background.h
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
// work, roughly a second, and it is the game's own doing rather than anything
// Wine or Proton adds; Windows shows it too.
//
// Black is what the game fades up from, so the flash disappears into the intro
// instead of announcing itself. Substituting the brush at registration is
// enough: nothing else reads hbrBackground, and the class is registered once.
//
// This hooks RegisterClassExA rather than patching the call site because the
// substitution needs no addresses and no prologue gating.
//
// Escha & Logy and Shallie use the same registration path with a different
// class and brush. Their measured `ElixirFramework`/WHITE_BRUSH pair is the
// second exact row in window_background.cpp's table.
namespace atfix {

// Install from DLL_PROCESS_ATTACH: the class is registered before the game's
// entry point runs, and a hook that arrives later has nothing left to change.
void installWindowBackgroundFix();

}  // namespace atfix
