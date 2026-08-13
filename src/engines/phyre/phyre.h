// SPDX-License-Identifier: MIT
#pragma once
//
// The PhyreEngine module: Atelier Ayesha DX only.
//
// Ayesha is the Dusk game built on the same old-MSVC-CRT/PhyreEngine lineage as
// the Arland DX ports, and its font-atlas and text-rendering code is the same
// code, function for function, as the one behind the Arland menu hitch. That is
// why every fix in this directory is an Arland port and why none of them means
// anything in Escha & Logy or Shallie -- those are on KTGL and live in
// `src/engines/ktgl/`.
//
// Everything here is gated on one of the two fingerprinted Ayesha executables.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>

#include "../../core/hook_util.h"

namespace atfix {

// Per-executable descriptor for the Phyre text path: the executable identity
// plus the RVAs of the three hooked entry points. Every row was derived
// separately; do not change an RVA here without re-deriving it.
//
// atlasUnlockRva names the two-instruction *stub* at lock+0x40, not the
// implementation behind it. Both are mapped; the stub is hooked so the hook
// sees the same argument convention as the Arland Totori/Meruru path. Because
// the stub's prologue window contains a jmp rel32 displacement it is not
// portable across builds, hence unlockExpected being per-row rather than a
// single shared constant.
struct PhyreGame {
  const char* executable;
  DWORD textSize;
  uintptr_t renderTextRva;
  uintptr_t atlasLockRva;
  uintptr_t atlasUnlockRva;
  std::array<BYTE, 16> unlockExpected;
  // The CS-guarded pad-create wrapper (core/pad_rescan.h). Its prologue is
  // displacement-free for only twelve bytes and then carries a per-build
  // rip-relative operand, so the window is per-row like unlockExpected.
  uintptr_t padCreateWrapperRva;
  std::array<BYTE, 16> padCreateExpected;
  uint8_t exeBuild;
};

}  // namespace atfix

namespace dusk {

// Recognizes the process as a fingerprinted Ayesha build, initializes MinHook,
// and installs whichever Phyre fixes are enabled. Idempotent. Returns true if
// the executable was recognized, whether or not any individual fix installed --
// each reports its own outcome to the log.
bool initializePhyreFixes();

// Present. For Phyre this is the atlas cache's entire lifetime, so it is not
// optional bookkeeping: see atlas_fix.h.
void phyreFrameTick();

}  // namespace dusk
