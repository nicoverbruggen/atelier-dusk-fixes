// SPDX-License-Identifier: MIT
#pragma once
//
// Shared low-level hook-installation infrastructure, mirroring the Arland
// project's hook_util.h: the per-executable hook descriptor, the prologue-match
// helper, and the two detour installers. Non-inline definitions live in
// hook_util.cpp.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace atfix {

// Per-executable hook descriptor: the executable identity (name, .text size)
// and the RVAs of the hooked engine functions.
//
// atlasUnlockRva names the two-instruction *stub* at lock+0x40, not the
// implementation behind it. Both are mapped (TECHNICAL.md 1.6); the stub is
// hooked so the hook sees the same argument convention as the Arland
// Totori/Meruru path. Because the stub's prologue window contains a jmp rel32
// displacement it is not portable across builds, hence unlockExpected being
// per-row rather than a single shared constant.
struct Game {
  const char* executable;
  DWORD textSize;
  uintptr_t queueDrainRva;
  uintptr_t renderTextRva;
  uintptr_t atlasLockRva;
  uintptr_t atlasUnlockRva;
  std::array<BYTE, 16> unlockExpected;
  uint8_t exeBuild;
};

// Each Dusk game ships an English build and a multilingual build (separate
// compiles, distinct RVAs). Rows whose RVAs are known for only one build stay
// gated on that build.
enum : uint8_t {
  BuildEnglish,
  BuildMultilingual,
};

// True if `target`'s bytes match `expected`, a verified prologue window.
template <size_t N>
inline bool matches(const BYTE* target, const std::array<BYTE, N>& expected) {
  return !std::memcmp(target, expected.data(), expected.size());
}

// Write a 14-byte absolute jmp to `target` at `destination`.
void writeAbsoluteJump(BYTE* destination, const void* target);

// Byte-patch detour: copies the prologue to a trampoline and writes an absolute
// jump over `target` (requires patchSize >= 14). Returns the trampoline in
// `original`. False on failure.
bool installDetour(BYTE* target, const void* replacement,
                   size_t patchSize, void** original);

// MinHook-based detour (MinHook owns the trampoline). False on failure. This is
// what the atlas hooks use: the hooked unlock is a 14-byte stub, too small for
// installDetour to patch without clobbering what follows.
bool installMinHookDetour(BYTE* target, const void* replacement, void** original);

}  // namespace atfix
