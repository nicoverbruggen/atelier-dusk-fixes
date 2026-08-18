// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#include <atomic>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <string>

#include "util.h"

namespace atfix {

// Lock-free by construction, following ReShade's logger (source/dll_log.cpp).
// Each thread formats its whole line into a buffer of its own and hands it to
// one WriteFile; nothing is shared but the file handle, and WriteFile on a
// handle opened for append is atomic per call, so lines from different threads
// interleave whole rather than mixing.
//
// The absence of a lock is the point. The crash filter writes its post-mortem
// through this same Log, from whichever thread faulted. Any lock here can be
// held by a different thread at that moment, and the report then waits on a
// thread that is not going to finish, so the process hangs instead of producing
// the one artifact the logger exists for.
//
// FILE_SHARE_READ lets a session be watched while the game holds the log open.
// Every line goes straight to the operating system; the crash filter calls
// flush() once so the final report reaches disk without paying that cost for
// every ordinary or verbose line.
class Log {

public:

  Log(const char* filename) {
    rotate(filename);
    m_file = CreateFileA(filename, FILE_APPEND_DATA,
      FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  }

  void flush() {
    if (m_file != INVALID_HANDLE_VALUE)
      FlushFileBuffers(m_file);
  }

  ~Log() {
    if (m_file != INVALID_HANDLE_VALUE)
      CloseHandle(m_file);
  }

  template<typename... Args>
  void operator () (const Args&... args) {
    if (m_file == INVALID_HANDLE_VALUE)
      return;
    const auto now = std::chrono::steady_clock::now();
    if (m_start.load(std::memory_order_relaxed) ==
        std::chrono::steady_clock::time_point{}) {
      std::chrono::steady_clock::time_point unset{};
      m_start.compare_exchange_strong(unset, now, std::memory_order_relaxed);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - m_start.load(std::memory_order_relaxed)).count();

    thread_local std::ostringstream line;
    line.str(std::string());
    line.clear();
    // std::dec guards against a previous line's sticky std::hex manipulator
    // bleeding into the timestamp.
    line << std::dec << '[' << std::setw(8) << ms << "] ";
    (line << ... << args) << "\r\n";

    const std::string text = line.str();
    DWORD written = 0;
    WriteFile(m_file, text.data(), static_cast<DWORD>(text.size()),
      &written, nullptr);
  }

private:

  HANDLE m_file = INVALID_HANDLE_VALUE;
  std::atomic<std::chrono::steady_clock::time_point> m_start{};

  // Keep the previous session's log (crash post-mortems included) as
  // <filename>.old instead of truncating it away on launch.
  static void rotate(const char* filename) {
    std::string previous = std::string(filename) + ".old";
    MoveFileExA(filename, previous.c_str(), MOVEFILE_REPLACE_EXISTING);
  }

};

// The process-wide log, defined in main.cpp.
extern Log log;

}  // namespace atfix

namespace dusk {

// WHY A USING-DECLARATION SITS IN THIS HEADER, and it is not a style choice.
//
// MSVC's <cmath> declares ::log at global scope. The engine modules also bring
// atfix::log into global scope, by putting `using atfix::log;` in an anonymous
// namespace that sits outside namespace atfix. Both names are then at global
// scope, so an unqualified log(...) written inside namespace dusk walks out of
// dusk, reaches global scope, and finds two candidates: error C2872, 'log':
// ambiguous symbol.
//
// MinGW does not declare ::log the same way, so the local build stays green and
// the break only appears on the Windows CI job. It has now cost two rounds of
// that -- once in phyre/atlas_fix.cpp, once in phyre/phyre.cpp -- and a per-file
// `using` fixes only the file someone remembered.
//
// This declaration makes the name a member of dusk itself, so lookup stops here
// and never reaches the ambiguous scope. Every engine file is covered, because a
// file that calls log() includes this header to get it.
using atfix::log;

}  // namespace dusk
