// SPDX-License-Identifier: MIT
//
// See config.h for the split between this file and the environment switches.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstring>

#include "config.h"
#include "game.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

const char* configPath() {
  static const std::array<char, MAX_PATH + 1> path = [] {
    std::array<char, MAX_PATH + 1> result = { };
    const DWORD pathLength = GetModuleFileNameA(nullptr, result.data(), MAX_PATH);
    // GetModuleFileNameA returns MAX_PATH exactly when it had to truncate, and
    // a truncated path is not the directory we want. Swapping the executable
    // name for the ini name has to fit as well. Failing either, the buffer is
    // cleared so this returns null: keeping the executable path would write
    // every setting into a file nothing reads, which fails silently instead of
    // visibly.
    char* slash = nullptr;
    if (pathLength && pathLength < MAX_PATH) {
      char* back = std::strrchr(result.data(), '\\');
      char* forward = std::strrchr(result.data(), '/');
      slash = !back || (forward && forward > back) ? forward : back;
    }
    const size_t used = slash ? size_t(slash + 1 - result.data()) : 0;
    if (slash && used + sizeof("dusk-fix.ini") <= result.size())
      std::memcpy(slash + 1, "dusk-fix.ini", sizeof("dusk-fix.ini"));
    else
      result[0] = '\0';

    // Only [Launcher] SkipLauncher is seeded eagerly. The per-feature keys are
    // seeded lazily by featureEnabled(), so that each one is written with the
    // capability matrix's default for the game it is actually running in, and
    // so a game that supports none of them never grows keys it would ignore.
    if (result[0] &&
        GetFileAttributesA(result.data()) == INVALID_FILE_ATTRIBUTES)
      WritePrivateProfileStringA("Launcher", "SkipLauncher", "false",
        result.data());
    return result;
  }();
  return path[0] ? path.data() : nullptr;
}

bool duskConfigBool(const char* section, const char* key, bool def) {
  const char* path = configPath();
  if (!path)
    return def;
  char value[16] = { };
  // \x01 is the "key absent" sentinel: an empty default cannot distinguish a
  // missing key from a key someone deliberately blanked.
  GetPrivateProfileStringA(section, key, "\x01", value, sizeof(value), path);
  if (value[0] == '\x01') {
    WritePrivateProfileStringA(section, key, def ? "true" : "false", path);
    return def;
  }
  return value[0] == 't' || value[0] == 'T' || value[0] == '1' ||
         value[0] == 'y' || value[0] == 'Y';
}

void logConfiguration() {
  const char* path = configPath();
  if (!path) {
    log("Config: dusk-fix.ini path could not be resolved; built-in defaults"
        " are in force");
    return;
  }
  log("Config: ", path);
  // Reported per feature rather than by dumping the file, so the line says what
  // is in force after the capability matrix has had its say. A feature the
  // running game does not support is hard off and has no ini key at all, which
  // is exactly what "unsupported" should look like in a log.
  static const struct { Feature feature; const char* name; } kReported[] = {
    { Feature::AtlasCache,      "AtlasCache" },
    { Feature::FieldEngineFix,  "FieldEngineFix" },
    { Feature::FieldStabilizer, "FieldStabilizer" },
  };
  for (const auto& row : kReported) {
    if (featureSupport(row.feature) == Support::Unsupported)
      log("Config:   ", row.name, " = unsupported on this game");
    else
      log("Config:   ", row.name, " = ",
        featureEnabled(row.feature) ? "on" : "off");
  }
}

}  // namespace atfix
