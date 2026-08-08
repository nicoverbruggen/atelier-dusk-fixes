// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>

#include "../vendor/minhook/include/MinHook.h"

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


/**
  * \brief Recursive mutex implementation
  *
  * Drop-in replacement for \c std::recursive_mutex that
  * uses Win32 critical sections.
  */
class recursive_mutex {

public:

  using native_handle_type = PCRITICAL_SECTION;

  recursive_mutex() {
    InitializeCriticalSection(&m_lock);
  }

  ~recursive_mutex() {
    DeleteCriticalSection(&m_lock);
  }

  recursive_mutex(const recursive_mutex&) = delete;
  recursive_mutex& operator = (const recursive_mutex&) = delete;

  void lock() {
    EnterCriticalSection(&m_lock);
  }

  void unlock() {
    LeaveCriticalSection(&m_lock);
  }

  bool try_lock() {
    return TryEnterCriticalSection(&m_lock);
  }

  native_handle_type native_handle() {
    return &m_lock;
  }

private:

  CRITICAL_SECTION m_lock;

};


/**
 * \brief SRW-based condition variable implementation
 *
 * Drop-in replacement for \c std::condition_variable that
 * uses Win32 condition variables on SRW locks.
 */
class condition_variable {

public:

  using native_handle_type = PCONDITION_VARIABLE;

  condition_variable() {
    InitializeConditionVariable(&m_cond);
  }

  condition_variable(condition_variable&) = delete;

  condition_variable& operator = (condition_variable&) = delete;

  void notify_one() {
    WakeConditionVariable(&m_cond);
  }

  void notify_all() {
    WakeAllConditionVariable(&m_cond);
  }

  void wait(std::unique_lock<mutex>& lock) {
    auto srw = lock.mutex()->native_handle();
    SleepConditionVariableSRW(&m_cond, srw, INFINITE, 0);
  }

  template<typename Predicate>
  void wait(std::unique_lock<mutex>& lock, Predicate pred) {
    while (!pred())
      wait(lock);
  }

  template<typename Clock, typename Duration>
  std::cv_status wait_until(std::unique_lock<mutex>& lock, const std::chrono::time_point<Clock, Duration>& time) {
    auto now = Clock::now();

    return (now < time)
      ? wait_for(lock, now - time)
      : std::cv_status::timeout;
  }

  template<typename Clock, typename Duration, typename Predicate>
  bool wait_until(std::unique_lock<mutex>& lock, const std::chrono::time_point<Clock, Duration>& time, Predicate pred) {
    if (pred())
      return true;

    auto now = Clock::now();
    return now < time && wait_for(lock, now - time, pred);
  }

  template<typename Rep, typename Period>
  std::cv_status wait_for(std::unique_lock<mutex>& lock, const std::chrono::duration<Rep, Period>& timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    auto srw = lock.mutex()->native_handle();

    return SleepConditionVariableSRW(&m_cond, srw, ms.count(), 0)
      ? std::cv_status::no_timeout
      : std::cv_status::timeout;
  }

  template<typename Rep, typename Period, typename Predicate>
  bool wait_for(std::unique_lock<mutex>& lock, const std::chrono::duration<Rep, Period>& timeout, Predicate pred) {
    bool result = pred();

    if (!result && wait_for(lock, timeout) == std::cv_status::no_timeout)
      result = pred();

    return result;
  }

  native_handle_type native_handle() {
    return &m_cond;
  }

private:

  CONDITION_VARIABLE m_cond;

};

}
