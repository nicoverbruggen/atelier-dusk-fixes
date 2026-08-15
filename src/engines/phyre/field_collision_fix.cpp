// SPDX-License-Identifier: MIT
//
// Implementation. What this demonstrates and why it takes this shape is in
// field_collision_fix.h; what is here is the per-build wiring and the notes
// that only mean anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "field_collision_fix.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/protection_transaction.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The `subss xmm3, xmm9` that computes the unclamped depth, and the int3
// padding after the routine that holds the stub. Both builds carry the same
// bytes at this site and the same 0x6b8 routine size, so the window is shared
// rather than per-row -- unusual in this module, and true only because the
// surrounding arithmetic is register-only and carries no displacement.
struct CollisionRvas {
  uintptr_t depth;
  uintptr_t padding;      // the routine's end, where its alignment fill starts
};

constexpr CollisionRvas kCollisionRvas[] = {
  { 0x733de0, 0x7341e8 },   // Atelier_Ayesha_EN.exe, routine 0x733b30..0x7341e8
  { 0x7562e0, 0x7566e8 },   // Atelier_Ayesha.exe,    routine 0x756030..0x7566e8
};

constexpr std::array<BYTE, 16> kDepthExpected = {
  0xf3, 0x41, 0x0f, 0x5c, 0xd9, 0xf3, 0x41, 0x0f,
  0x59, 0xf0, 0x41, 0x0f, 0x28, 0xd0, 0xf3, 0x41
};

// The displaced instruction is exactly the width of the jump that replaces it,
// so unlike most patches of this shape there is no filler to write.
constexpr size_t kSiteLength = 5;
constexpr size_t kJumpLength = 5;
// The constant lives past the three instructions, so the stub's entry point and
// its first instruction are the same address.
constexpr size_t kGainOffset = 18;
constexpr size_t kStubLength = kGainOffset + sizeof(float);

std::atomic<bool> g_installed{false};

// Nothing here validates the number beyond requiring it to be positive and
// finite. A gain of 1 is the honest no-op and a useful control: it proves the
// stub is running without changing what the game does.
float requestedGain() {
  const char* value = std::getenv("DUSK_COLLIDE_GAIN");
  if (!value || !value[0])
    return 0.0f;
  const double parsed = std::strtod(value, nullptr);
  if (!(parsed > 0.0) || parsed > 1000.0)
    return 0.0f;
  return float(parsed);
}

// The clamp and the gain are the same patch with one opcode byte and one
// constant changed: 0x5f is maxss, 0x59 is mulss. Sharing the builder is what
// keeps that true -- two copies would drift, and the whole point of the
// diagnostic is that it exercises the instruction the fix replaces.
bool installStub(BYTE* base, uint8_t exeBuild, BYTE opcode, float constant,
                 const char* what) {
  const CollisionRvas& rvas = kCollisionRvas[exeBuild == BuildEnglish ? 0 : 1];
  BYTE* site = base + rvas.depth;
  BYTE* stub = base + rvas.padding;

  if (!matches(site, kDepthExpected)) {
    log("Collision ", what, " window mismatch at 0x", std::hex, rvas.depth,
        std::dec, "; not patching");
    return false;
  }
  // The padding is what makes this allocation-free, so it is checked rather
  // than assumed: a build that fills alignment differently must decline instead
  // of writing a stub over whatever is actually there.
  for (size_t i = 0; i < kStubLength; ++i) {
    if (stub[i] != 0xCC) {
      log("Collision ", what, ": the fill after the routine is not int3 at +",
          std::dec, i, "; not patching");
      return false;
    }
  }

  // Build the stub in a buffer first, so the live bytes are written once and
  // the game never executes a half-assembled sequence.
  //
  // THE CODE STARTS AT OFFSET ZERO AND THE CONSTANT SITS AFTER IT. Putting the
  // float first reads better and cost a crash: the jump below targets the stub
  // address, so with the float at the front the processor decoded it as
  // instructions -- 5.0f is 00 00 a0 40, and `00 00` is `add [rax], al`, a
  // write through whatever rax held. Entry at offset zero means the jump target
  // and the first instruction cannot disagree.
  BYTE image[kStubLength] = {};
  std::memcpy(image, site, kSiteLength);                    // [+0] displaced subss

  // maxss or mulss against [rip+disp32]. The displacement is measured from the
  // end of this instruction, at stub+13, forward to the constant at stub+18.
  image[5] = 0xf3; image[6] = 0x0f; image[7] = opcode; image[8] = 0x1d;
  const int32_t toGain = int32_t(kGainOffset) - 13;
  std::memcpy(image + 9, &toGain, sizeof(toGain));

  // jmp back to the instruction after the one we displaced.
  image[13] = 0xe9;
  const int32_t back = int32_t((site + kSiteLength) - (stub + kGainOffset));
  std::memcpy(image + 14, &back, sizeof(back));

  std::memcpy(image + kGainOffset, &constant, sizeof(constant));

  {
    ProtectionTransaction protection;
    if (!protection.change(stub, kStubLength, PAGE_EXECUTE_READWRITE)) {
      log("FIXES ", what, "=failed (stub page protection)");
      return false;
    }
    std::memcpy(stub, image, kStubLength);
    protection.rollback();
  }

  // The site last, so the jump only becomes reachable once its destination is
  // already assembled. A failure between the two leaves an unreferenced stub in
  // padding and the game running exactly as before.
  {
    ProtectionTransaction protection;
    if (!protection.change(site, kSiteLength, PAGE_EXECUTE_READWRITE)) {
      log("FIXES ", what, "=failed (site page protection)");
      return false;
    }
    BYTE patch[kSiteLength];
    patch[0] = 0xe9;
    const int32_t toStub = int32_t(stub - (site + kJumpLength));
    std::memcpy(patch + 1, &toStub, sizeof(toStub));
    std::memcpy(site, patch, kSiteLength);
    protection.rollback();
  }

  FlushInstructionCache(GetCurrentProcess(), stub, kStubLength);
  FlushInstructionCache(GetCurrentProcess(), site, kSiteLength);

  g_installed.store(true, std::memory_order_relaxed);
  log("FIXES ", what, "=active at 0x", std::hex, rvas.depth, " stub=0x",
      rvas.padding, std::dec, " constant=", constant);
  return true;
}

}  // namespace

bool installFieldCollision(BYTE* base, uint8_t exeBuild) {
  if (g_installed.load(std::memory_order_relaxed))
    return true;
  if (!base)
    return false;

  // The diagnostic wins when it is asked for, because both patches claim the
  // same five bytes and a session that sets the variable is asking to see the
  // defect rather than to have it repaired. Saying so is the point: a silent
  // choice here would look like the fix failing.
  const float gain = requestedGain();
  if (gain > 0.0f) {
    log("FIXES field_character_pull=superseded by DUSK_COLLIDE_GAIN, so this"
        " session demonstrates the defect instead of fixing it");
    return installStub(base, exeBuild, 0x59, gain, "collide_demo");
  }

  if (featureSupport(Feature::FieldCharacterPull) == Support::Unsupported) {
    log("FIXES field_character_pull=not_applicable");
    return false;
  }
  if (!featureEnabled(Feature::FieldCharacterPull)) {
    log("FIXES field_character_pull=off");
    return false;
  }
  return installStub(base, exeBuild, 0x5f, 0.0f, "field_character_pull");
}

}  // namespace atfix
