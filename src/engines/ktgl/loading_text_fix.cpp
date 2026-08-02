// SPDX-License-Identifier: MIT
//
// See loading_text_fix.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstring>

#include "loading_text_fix.h"

#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/mem.h"

namespace atfix {
extern Log log;   // main.cpp
}

namespace {

using atfix::log;
using atfix::matches;
using atfix::readableRange;

// The shipped literal, including its terminator. This is the gate: the RVA rows
// in ktgl.cpp say where to look, this says what has to be there. A build that
// has been repatched, or an RVA that has drifted, fails here rather than getting
// 22 bytes of unrelated .rdata overwritten.
constexpr std::array<BYTE, 22> kShipped = {
  'L', 'o', 'a', 'd', 'n', 'i', 'n', 'g', ' ', 's', 'y',
  's', 't', 'e', 'm', ' ', 'd', 'a', 't', 'a', '.', 0x00,
};

// The correction, at exactly the same length. "Loading" is one character shorter
// than "Loadning", so the string ends a byte earlier and the trailing byte is
// cleared rather than left holding the old '.'; the region's size is unchanged
// and nothing downstream of it moves.
constexpr std::array<BYTE, 22> kCorrected = {
  'L', 'o', 'a', 'd', 'i', 'n', 'g', ' ', 's', 'y', 's',
  't', 'e', 'm', ' ', 'd', 'a', 't', 'a', '.', 0x00, 0x00,
};

static_assert(kShipped.size() == kCorrected.size(),
  "the replacement must occupy exactly the region the literal already owns");

}  // namespace

namespace dusk {

bool installLoadingTextFix(BYTE* base, const atfix::KtglGame& game) {
  if (!atfix::featureEnabled(atfix::Feature::LoadingTextTypo)) {
    log("FIXES loading_text=off");
    return false;
  }
  if (!game.loadingTextRva) {
    log("FIXES loading_text=unavailable (no address row for ", game.executable,
        "; the string has not been located in this build)");
    return false;
  }

  BYTE* target = base + game.loadingTextRva;
  if (!readableRange(reinterpret_cast<uintptr_t>(target), kShipped.size())) {
    log("FIXES loading_text=declined (rva 0x", std::hex, game.loadingTextRva,
        std::dec, " is not mapped)");
    return false;
  }
  if (!matches(target, kShipped)) {
    log("FIXES loading_text=declined (rva 0x", std::hex, game.loadingTextRva,
        std::dec, " does not hold the shipped literal)");
    return false;
  }

  // .rdata is mapped read-only, so this is not the belt-and-braces call it is in
  // field_physics.cpp -- without it the store faults.
  DWORD previous = 0;
  if (!VirtualProtect(target, kCorrected.size(), PAGE_READWRITE, &previous)) {
    log("FIXES loading_text=declined (page at rva 0x", std::hex,
        game.loadingTextRva, std::dec, " cannot be made writable)");
    return false;
  }
  std::memcpy(target, kCorrected.data(), kCorrected.size());
  DWORD ignored = 0;
  VirtualProtect(target, kCorrected.size(), previous, &ignored);

  log("FIXES loading_text=active rva=0x", std::hex, game.loadingTextRva,
      std::dec, " \"Loadning system data.\" -> \"Loading system data.\"");
  return true;
}

}  // namespace dusk
