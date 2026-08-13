// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>

// Portable caller return address, used by the hook functions to recover the
// game call site (its RVA distinguishes call sites of a shared hooked routine).
// Must be a macro: wrapping the intrinsic in a function would return the
// wrapper's own return address. Returns void*; call sites reinterpret_cast it to
// uintptr_t. The enclosing function must not be inlined, which holds because the
// hook targets are address-taken (passed to MinHook).
#if defined(_MSC_VER)
  #include <intrin.h>
  #pragma intrinsic(_ReturnAddress)
  #define duskReturnAddress() (_ReturnAddress())
#else
  #define duskReturnAddress() (__builtin_return_address(0))
#endif

namespace atfix {

// ---- the startup-movie budget ----------------------------------------------
//
// Both movie skips count plays rather than inspecting which movie is playing,
// and the reason is the in-game Movies gallery. Every game reaches its movie
// player through one routine, so a rule keyed on the movie's identity that
// skips the opening at boot also skips it when the player deliberately picks it
// from the gallery. Counting cannot make that mistake: the budget is spent
// during boot and the gallery is always afterwards.
//
// It also retires a question static analysis could not settle. Ayesha's table
// carries both `opening` and `avantitle` and no constant-index call site says
// which one boots; Escha and Shallie number the same files differently again.
// Under this rule it does not matter what the boot movie is called or where it
// sits in the table -- only how many play before the player has control.
//
// A count rather than a time window, which was the other candidate: a window
// has to assume how long booting takes, and would either expire early on a slow
// machine or reach too far into play on a fast one. A budget assumes nothing.
//
// The failure modes stay asymmetric, which is what makes a small budget right.
// Too small and a movie plays: the feature did not fully work, visibly and
// harmlessly. Too large and it eats one the player asked for, which is a bug
// they cannot explain.
//
// Returns true while budget remains, and consumes one. The counter is a
// function-local static in an inline function, so all callers share one -- which
// is correct, since only one engine module is ever live in a process.
inline bool consumeStartupMovieBudget(int budget, int* playedOut = nullptr) {
  static std::atomic<int> played{0};
  const int n = played.fetch_add(1, std::memory_order_relaxed);
  if (playedOut)
    *playedOut = n + 1;
  return n < budget;
}

/**
 * \brief SRW-based mutex implementation
 *
 * Drop-in replacement for \c std::mutex that uses Win32
 * SRW locks, which are implemented with \c futex in wine.
 */
class mutex {

public:

  using native_handle_type = PSRWLOCK;

  mutex() { }

  mutex(const mutex&) = delete;
  mutex& operator = (const mutex&) = delete;

  void lock() {
    AcquireSRWLockExclusive(&m_lock);
  }

  void unlock() {
    ReleaseSRWLockExclusive(&m_lock);
  }

  bool try_lock() {
    return TryAcquireSRWLockExclusive(&m_lock);
  }

  native_handle_type native_handle() {
    return &m_lock;
  }

private:

  SRWLOCK m_lock = SRWLOCK_INIT;

};


}
