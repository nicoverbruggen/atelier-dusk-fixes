// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// shadow_tap.h; what is here is the per-build wiring and the notes that only
// mean anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "shadow_tap.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/protection_transaction.h"
#include "../../core/shadow_res.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The `movss xmm2, [rip+disp32]` that loads the vanilla 1/1024, and enough of
// what follows to be sure of the site rather than of the opcode. The window
// runs into the `lea rdx` for the parameter name, so a build whose surrounding
// code moved fails the check even if an unrelated movss happens to sit at the
// same address.
//
// Both rows were derived the same way and separately: find the one reference to
// the "tapScale" string, take the `lea` that makes it, step back eight bytes to
// the load, and read the float its displacement resolves to. Both builds gave
// 0.0009765625. Do not change an RVA here without re-deriving it.
struct TapRvas {
  uintptr_t movss;
  std::array<BYTE, 16> expected;
};

constexpr TapRvas kTapRvas[] = {
  // Atelier_Ayesha_EN.exe -- float at 0xbd445c, name string at 0xbd7188
  { 0x1b1a63,
    { 0xf3, 0x0f, 0x10, 0x15, 0xf1, 0x29, 0xa2, 0x00,
      0x48, 0x8d, 0x15, 0x16, 0x57, 0xa2, 0x00, 0x45 } },
  // Atelier_Ayesha.exe -- float at 0xc10c0c, name string at 0xc13948
  { 0x1b6563,
    { 0xf3, 0x0f, 0x10, 0x15, 0xa1, 0xa6, 0xa5, 0x00,
      0x48, 0x8d, 0x15, 0xd6, 0xd3, 0xa5, 0x00, 0x45 } },
};

// The instruction is eight bytes and its displacement is the last four, so the
// program counter the displacement is measured from is the instruction's own
// address plus eight.
constexpr size_t kMovssLength = 8;
constexpr size_t kDispOffset = 4;

std::atomic<bool> g_installed{false};

// A page the mod owns, near enough to the module for a rip-relative operand to
// reach it. Asking for an address rather than taking whatever the allocator
// offers is the whole point: the default would land wherever there is room,
// which on this platform is routinely further than 2 GB away.
float* allocateNearModule(BYTE* base) {
  // The module's own headers, so the search starts past the image rather than
  // past a number someone guessed and a later build outgrew.
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return nullptr;
  const auto* nt =
    reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return nullptr;
  const uintptr_t imageSize = nt->OptionalHeader.SizeOfImage;

  constexpr uintptr_t kGranularity = 0x10000;
  uintptr_t start = (reinterpret_cast<uintptr_t>(base) + imageSize +
                     kGranularity - 1) & ~(kGranularity - 1);
  // Forward first, then backward. Either direction is in range; trying both
  // means a fully occupied span on one side is not a failure.
  for (int pass = 0; pass < 2; ++pass) {
    for (int step = 0; step < 512; ++step) {
      const uintptr_t candidate = pass == 0
        ? start + uintptr_t(step) * kGranularity
        : reinterpret_cast<uintptr_t>(base) - uintptr_t(step + 1) * kGranularity;
      void* page = VirtualAlloc(reinterpret_cast<void*>(candidate), sizeof(float),
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (page)
        return static_cast<float*>(page);
    }
  }
  return nullptr;
}

}  // namespace

bool installShadowTapScale(BYTE* base, uint8_t exeBuild) {
  if (g_installed.load(std::memory_order_relaxed))
    return true;
  if (!base)
    return false;

  // The twin is the reason this exists. Rescaling the kernel while the engine
  // still renders into its own 1024 map would narrow the filter below one
  // texel, which trades a soft edge for an aliased one -- worse than doing
  // nothing. shadowResWanted covers the multiplier, the game and the
  // high-resolution prerequisite in one answer.
  if (!shadowResWanted())
    return false;

  const TapRvas& rvas = kTapRvas[exeBuild == BuildEnglish ? 0 : 1];
  BYTE* target = base + rvas.movss;
  if (!matches(target, rvas.expected)) {
    log("Shadow tap-scale window mismatch at 0x", std::hex, rvas.movss,
        std::dec, "; not patching, so the enlarged map keeps the vanilla"
        " one-texel-at-1024 filter");
    return false;
  }

  float* slot = allocateNearModule(base);
  if (!slot) {
    log("FIXES shadow_tapscale=failed (no page near the module)");
    return false;
  }

  // The vanilla value is one texel of a 1024 map expressed in UV, so one texel
  // of the enlarged map is the same statement with the new size in it.
  const unsigned int resolution = shadowMapResolution();
  *slot = 1.0f / float(resolution);

  const uintptr_t rip = reinterpret_cast<uintptr_t>(target) + kMovssLength;
  const intptr_t displacement =
    intptr_t(reinterpret_cast<uintptr_t>(slot)) - intptr_t(rip);
  if (displacement > INT32_MAX || displacement < INT32_MIN) {
    log("FIXES shadow_tapscale=failed (page is ", std::dec,
        displacement / (1024 * 1024), " MB away, out of rip-relative reach)");
    VirtualFree(slot, 0, MEM_RELEASE);
    return false;
  }

  ProtectionTransaction protection;
  if (!protection.change(target, kMovssLength, PAGE_EXECUTE_READWRITE)) {
    log("FIXES shadow_tapscale=failed (page protection)");
    VirtualFree(slot, 0, MEM_RELEASE);
    return false;
  }
  const int32_t narrowed = int32_t(displacement);
  std::memcpy(target + kDispOffset, &narrowed, sizeof(narrowed));
  // Let the transaction restore the original protection on the way out. The
  // write is the whole patch, so there is nothing to keep the page writable
  // for, and committing here would leave engine code permanently writable.
  protection.rollback();
  FlushInstructionCache(GetCurrentProcess(), target, kMovssLength);

  g_installed.store(true, std::memory_order_relaxed);
  log("FIXES shadow_tapscale=active tap=1/", std::dec, resolution,
      " (was 1/1024) at 0x", std::hex, rvas.movss, std::dec);
  return true;
}

}  // namespace atfix
