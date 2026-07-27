// SPDX-License-Identifier: MIT
//
// Definitions for the shared hook-installation helpers declared in hook_util.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>

#include <cstdint>
#include <cstring>

#include "hook_util.h"
#include "../../vendor/minhook/include/MinHook.h"

namespace atfix {

bool currentModuleIdentity(ModuleIdentity& out) {
  HMODULE module = GetModuleHandleW(nullptr);
  if (!module)
    return false;
  if (!GetModuleFileNameA(module, out.path, sizeof(out.path)))
    return false;
  const char* back = std::strrchr(out.path, '\\');
  const char* forward = std::strrchr(out.path, '/');
  const char* sep = back > forward ? back : forward;
  out.name = sep ? sep + 1 : out.path;

  auto* base = reinterpret_cast<BYTE*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return false;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if (!std::memcmp(section->Name, ".text", 5)) {
      out.textSize = section->Misc.VirtualSize;
      break;
    }
  }
  out.base = base;
  return true;
}

void writeAbsoluteJump(BYTE* destination, const void* target) {
  destination[0] = 0xff;
  destination[1] = 0x25;
  std::memset(destination + 2, 0, 4);
  const uintptr_t address = reinterpret_cast<uintptr_t>(target);
  std::memcpy(destination + 6, &address, sizeof(address));
}

bool installDetour(BYTE* target, const void* replacement,
                   size_t patchSize, void** original) {
  if (patchSize < 14)
    return false;
  auto* trampoline = static_cast<BYTE*>(VirtualAlloc(
    nullptr, patchSize + 14, MEM_COMMIT | MEM_RESERVE,
    PAGE_EXECUTE_READWRITE));
  if (!trampoline)
    return false;
  std::memcpy(trampoline, target, patchSize);
  writeAbsoluteJump(trampoline + patchSize, target + patchSize);
  FlushInstructionCache(GetCurrentProcess(), trampoline, patchSize + 14);
  *original = trampoline;

  DWORD oldProtection = 0;
  if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
    VirtualFree(trampoline, 0, MEM_RELEASE);
    *original = nullptr;
    return false;
  }
  writeAbsoluteJump(target, replacement);
  std::memset(target + 14, 0x90, patchSize - 14);
  FlushInstructionCache(GetCurrentProcess(), target, patchSize);
  DWORD ignored = 0;
  VirtualProtect(target, patchSize, oldProtection, &ignored);
  return true;
}

bool installMinHookDetour(BYTE* target, const void* replacement,
                          void** original) {
  const MH_STATUS created = MH_CreateHook(
    target, const_cast<void*>(replacement), original);
  if (created != MH_OK)
    return false;
  return MH_EnableHook(target) == MH_OK;
}

}  // namespace atfix
