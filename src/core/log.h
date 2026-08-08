// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#include <chrono>
#include <fstream>
#include <iomanip>

#include "util.h"

namespace atfix {

class Log {

public:

  Log(const char* filename)
  : m_file((rotate(filename), filename), std::ios::out | std::ios::trunc) {

  }

  template<typename... Args>
  void operator () (const Args&... args) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);
    if (m_start == std::chrono::steady_clock::time_point{})
      m_start = now;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - m_start).count();
    // std::dec guards against a previous line's sticky std::hex manipulator
    // bleeding into the timestamp.
    m_file << std::dec << '[' << std::setw(8) << ms << "] ";
    (m_file << ... << args) << std::endl;
  }

private:

  mutex         m_mutex;
  std::ofstream m_file;
  std::chrono::steady_clock::time_point m_start;

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
