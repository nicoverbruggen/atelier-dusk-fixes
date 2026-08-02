// SPDX-License-Identifier: MIT
#pragma once
//
// Shared low-level hook-installation infrastructure, mirroring the Arland
// project's hook_util.h: the prologue-match helper, the two detour installers,
// and the loaded-module identity every engine module gates on. Non-inline
// definitions live in hook_util.cpp.
//
// Engine-specific hook descriptors do NOT live here. The Dusk trilogy spans two
// engines with disjoint address packs, so each engine module owns its own table:
// see `engine/phyre/phyre.h` (Ayesha) and `engine/ktgl/ktgl.h` (Escha & Logy, Shallie).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace atfix {

// Each Dusk game ships an English build and a multilingual build (separate
// compiles, distinct RVAs). Rows whose RVAs are known for only one build stay
// gated on that build.
enum : uint8_t {
  BuildEnglish,
  BuildMultilingual,
};

// The running executable, as the fingerprint gate sees it. `textSize` is .text's
// VirtualSize read from the *loaded* headers rather than the file, so a build
// that is packed on disk still matches once its stub has decrypted the section
// (not currently a factor for Ayesha, but the Arland project hit it with
// Meruru's SteamStub wrapper).
struct ModuleIdentity {
  BYTE* base = nullptr;
  const char* name = nullptr;   // base name, points into `path`
  DWORD textSize = 0;
  char path[MAX_PATH] = {};
};

// Fills `out` for the current process image. False if the headers cannot be
// read, in which case nothing in `out` is meaningful.
bool currentModuleIdentity(ModuleIdentity& out);

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
