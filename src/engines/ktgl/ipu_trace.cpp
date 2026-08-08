// SPDX-License-Identifier: MIT
//
// `DUSK_IPU_TRACE`: what 2D image Escha & Logy or Shallie just loaded, and who
// asked for it. A diagnostic. It suppresses nothing and changes nothing the game
// does, and it is off unless the switch is set.
//
// WHY IT EXISTS. Both games load every 2D image through one routine that takes a
// row of an image table and builds `<dir><name>` from it. Which rows load says
// little on its own; the caller is what identifies the subsystem, and that was
// the question static analysis kept failing to answer. The startup logo sequence
// is the worked example: its three states are referenced only as 32-bit
// rip-relative `lea`s inside `Title`'s initializer, so no absolute-address
// search reaches them and `callsites` reports none of them. One traced boot
// showed both logos arriving from a single call site 1.3 seconds apart, and the
// sequence fell out of that in minutes. See logo_skip.cpp for what it became.
//
// It also settles a cheaper question every time it runs: whether an image is
// requested at all. A row that never appears is not being drawn by this routine,
// whatever the table implies.
//
// The output is one line per distinct row, capped, because these games have 180
// and 255 rows and load most of them over a session. The cap announces itself
// rather than going quiet, since a truncated list that looks complete is worse
// than no list.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "ipu_trace.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/util.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// (Ipu*, row, flag). The return value belongs to a routine this one tail-calls,
// so the trace declares one and hands it straight back.
using IpuLoadProc = uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, int32_t, BYTE);

IpuLoadProc originalIpuLoad = nullptr;

// One row of the image table. Both games use this layout; only the row count
// differs, and the trace only ever reads the name.
struct IpuRow {
  uint32_t width;
  uint32_t height;
  float xRate;
  float yRate;
  const char* dir;
  const char* name;
};
static_assert(sizeof(IpuRow) == 0x20, "the image table has a 0x20-byte stride");

constexpr int kMaxRows = 256;
constexpr int kRowLogLimit = 48;

std::atomic<bool> tracing{false};
std::atomic<uintptr_t> imageBase{0};
std::atomic<const IpuRow*> imageTable{nullptr};
std::atomic<int32_t> lastTableRow{-1};
std::atomic<uint64_t> seenRows[kMaxRows / 64];
std::atomic<int> rowsLogged{0};

const char* rowName(int32_t row) {
  const IpuRow* table = imageTable.load(std::memory_order_relaxed);
  if (!table || row < 0 || row > lastTableRow.load(std::memory_order_relaxed))
    return nullptr;
  return table[row].name;
}

void noteRow(int32_t row, uintptr_t callerRva) {
  if (row < 0 || row >= kMaxRows)
    return;
  const uint64_t bit = uint64_t(1) << (row % 64);
  if (seenRows[row / 64].fetch_or(bit, std::memory_order_relaxed) & bit)
    return;
  const int n = rowsLogged.fetch_add(1, std::memory_order_relaxed);
  if (n > kRowLogLimit)
    return;
  if (n == kRowLogLimit) {
    log("IPU: ", std::dec, kRowLogLimit,
        " distinct rows logged; further rows are not listed");
    return;
  }
  const char* name = rowName(row);
  log("IPU: row ", std::dec, row, " ", name ? name : "(no image)",
      " caller=0x", std::hex, callerRva, std::dec);
}

uintptr_t STDMETHODCALLTYPE tracedIpuLoad(uintptr_t self, int32_t row,
                                          BYTE flag) {
  if (tracing.load(std::memory_order_relaxed)) {
    const uintptr_t base = imageBase.load(std::memory_order_relaxed);
    const uintptr_t caller = uintptr_t(duskReturnAddress());
    noteRow(row, caller >= base ? caller - base : caller);
  }
  return originalIpuLoad(self, row, flag);
}

// Byte-identical in all four builds: `push rdi / sub rsp, 0x50 / mov
// [rsp+0x20], -2`. The routine is a vtable slot, but not the same slot in both
// games -- Escha's is 3 and Shallie's is 14 -- so the address pack carries the
// body and this window confirms it.
constexpr std::array<BYTE, 16> kIpuLoadExpected = {
  0x40, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0xc7,
  0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48,
};

// The routine's own row bound, `cmp esi, imm32`, at the same offset in all four
// builds. Reading it confirms the descriptor's row count against the executable
// actually running, which is what keeps the trace from indexing a table it has
// misidentified.
constexpr size_t kBoundOffset = 0x28;
constexpr std::array<BYTE, 2> kBoundOpcode = { 0x81, 0xfe };

}  // namespace

bool installIpuTrace(BYTE* base, const KtglGame& game) {
  const char* enabled = std::getenv("DUSK_IPU_TRACE");
  if (!enabled || enabled[0] == '0')
    return false;
  if (!game.ipuLoadRva || !game.ipuTableRva || !game.ipuTableRows) {
    log("IPU trace: no address row for this build");
    return false;
  }

  BYTE* target = base + game.ipuLoadRva;
  if (!matches(target, kIpuLoadExpected)) {
    log("IPU trace: prologue mismatch at 0x", std::hex, game.ipuLoadRva,
        std::dec, "; not installing");
    return false;
  }
  if (std::memcmp(target + kBoundOffset, kBoundOpcode.data(), 2) != 0) {
    log("IPU trace: no row bound at +0x", std::hex, kBoundOffset, std::dec,
        "; not installing");
    return false;
  }
  uint32_t lastRow = 0;
  std::memcpy(&lastRow, target + kBoundOffset + 2, sizeof(lastRow));
  if (lastRow + 1 != game.ipuTableRows) {
    log("IPU trace: row bound 0x", std::hex, lastRow,
        " disagrees with the descriptor's 0x", game.ipuTableRows - 1, std::dec,
        "; not installing");
    return false;
  }

  imageBase.store(reinterpret_cast<uintptr_t>(base), std::memory_order_relaxed);
  imageTable.store(reinterpret_cast<const IpuRow*>(base + game.ipuTableRva),
                   std::memory_order_relaxed);
  lastTableRow.store(int32_t(lastRow), std::memory_order_relaxed);

  if (!installMinHookDetour(target, reinterpret_cast<void*>(&tracedIpuLoad),
                            reinterpret_cast<void**>(&originalIpuLoad))) {
    log("IPU trace: install failed");
    return false;
  }

  tracing.store(true, std::memory_order_relaxed);
  log("IPU trace: active load=0x", std::hex, game.ipuLoadRva, " table=0x",
      game.ipuTableRva, std::dec, " rows=", game.ipuTableRows,
      " (nothing is changed)");
  return true;
}

}  // namespace atfix
