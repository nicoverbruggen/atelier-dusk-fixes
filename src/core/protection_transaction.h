// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>

namespace atfix {

// One reversible page-protection change. The Win32 call is injected so the
// production state machine's open/rollback failures stay permanently testable.
class ProtectionTransaction {
public:
  using ProtectProc = BOOL (WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);

  explicit ProtectionTransaction(ProtectProc protect = ::VirtualProtect)
    : protect_(protect) {}

  ~ProtectionTransaction() {
    if (active_ && !committed_ && !rollbackAttempted_)
      rollback();
  }

  ProtectionTransaction(const ProtectionTransaction&) = delete;
  ProtectionTransaction& operator=(const ProtectionTransaction&) = delete;

  bool change(void* address, size_t size, DWORD protection) {
    if (!protect_ || !address || !size || active_ || committed_ ||
        rollbackAttempted_)
      return false;
    if (!protect_(address, size, protection, &originalProtection_))
      return false;
    address_ = address;
    size_ = size;
    active_ = true;
    return true;
  }

  bool rollback() {
    if (committed_)
      return false;
    if (!active_)
      return true;
    if (rollbackAttempted_)
      return rollbackComplete_;
    rollbackAttempted_ = true;
    DWORD ignored = 0;
    rollbackComplete_ = protect_(
      address_, size_, originalProtection_, &ignored) != FALSE;
    if (rollbackComplete_)
      active_ = false;
    return rollbackComplete_;
  }

  void commit() { committed_ = true; }

private:
  ProtectProc protect_ = nullptr;
  void* address_ = nullptr;
  size_t size_ = 0;
  DWORD originalProtection_ = 0;
  bool active_ = false;
  bool committed_ = false;
  bool rollbackAttempted_ = false;
  bool rollbackComplete_ = true;
};

}  // namespace atfix
