// SPDX-License-Identifier: MIT
//
// See config.h for the split between this file and the environment switches.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "game.h"
#include "log.h"
#include "sharpen.h"
#include "smaa.h"
#include "supersample.h"

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

int duskConfigInt(const char* section, const char* key, int def) {
  const char* path = configPath();
  if (!path)
    return def;
  char value[16] = { };
  GetPrivateProfileStringA(section, key, "\x01", value, sizeof(value), path);
  if (value[0] == '\x01') {
    char seed[16] = { };
    wsprintfA(seed, "%d", def);
    WritePrivateProfileStringA(section, key, seed, path);
    return def;
  }
  return std::atoi(value);
}

// Read once and cached, so a call on a hot path costs a load. The environment
// wins over the ini because it is the switch a developer sets for one run.
bool verboseLogging() {
  static const bool on = [] {
    const char* env = std::getenv("DUSK_VERBOSE_LOG");
    if (env)
      return env[0] != '0';
    return duskConfigBool("Diagnostics", "VerboseLogging", false);
  }();
  return on;
}

// Keys retired along with the features they configured.
//
// They are left exactly where they are. The file belongs to whoever owns the
// game, and quietly editing it is a poor way to explain that a setting went
// away; a line in the log is a better one, and the log is what a bug report
// already carries. So these are ignored, once, out loud.
//
// A player's ini outlives the code that read it.
//
// ADD TO THIS IN THE SAME CHANGE that removes an option.
struct RetiredKey { const char* section; const char* key; };
constexpr RetiredKey kRetiredKeys[] = {
  { "Interface", "PadRescanBackoff" },      // now keyless: a fix, not a setting
  { "Rendering", "SupersamplingSharpen" },
};

void warnRetiredKeys(const char* path) {
  for (const RetiredKey& retired : kRetiredKeys) {
    char value[8] = { };
    GetPrivateProfileStringA(retired.section, retired.key, "\x01", value,
      sizeof(value), path);
    if (value[0] == '\x01')
      continue;
    log("Config: ignored [", retired.section, "] ", retired.key,
      " -- retired, this key does nothing");
  }
}

void logConfiguration() {
  const char* path = configPath();
  if (!path) {
    log("Config: dusk-fix.ini path could not be resolved; built-in defaults"
        " are in force");
    return;
  }
  // Before the lines below, so a reader meets the warning first.
  warnRetiredKeys(path);
  log("Config: ", path);
  // Reported per feature rather than by dumping the file, so these lines say
  // what is in force after environment overrides and the capability matrix.
  // Unsupported is distinct from off: an ini key or environment variable
  // cannot activate that feature for this game.
  static const struct { Feature feature; const char* name; } kSwitches[] = {
    { Feature::AtlasVerify,      "AtlasVerify" },
    { Feature::TargetCensus,     "TargetCensus" },
    { Feature::HighResRendering, "HighResolution" },
    { Feature::AtlasCache,       "AtlasCache" },
    { Feature::FieldEngineFix,   "FieldEngineFix" },
    { Feature::WorldMapCursor,   "WorldMapCursor" },
    { Feature::SkipStartupLogos, "SkipLogos" },
    { Feature::SkipIntroMovie,   "SkipIntroMovie" },
    { Feature::LoadingTextTypo,  "LoadingTextCorrection" },
    { Feature::SystemSaveGuard,  "SystemSaveGuard" },
    { Feature::PadRescanBackoff, "PadRescanBackoff" },
    { Feature::SynthesisAnimationRate, "SynthesisAnimationRate" },
  };
  for (const auto& row : kSwitches) {
    if (featureSupport(row.feature) == Support::Unsupported)
      log("Config:   ", row.name, " = unsupported on this game");
    else
      log("Config:   ", row.name, " = ",
        featureEnabled(row.feature) ? "on" : "off");
  }

  if (featureSupport(Feature::Smaa) == Support::Unsupported) {
    log("Config:   SMAA = unsupported on this game");
  } else {
    log("Config:   SMAA = ", smaaEnabled() ? "on" : "off",
        "; pre-UI = ", smaaPreUiEnabled() ? "on" : "off");
  }

  if (featureSupport(Feature::Supersampling) == Support::Unsupported) {
    log("Config:   Supersampling = unsupported on this game");
  } else {
    log("Config:   Supersampling = ", ssaaPercent(), "%");
  }

  log("Config:   Sharpen = ",
      static_cast<unsigned int>(sharpenAmount() * 100.0f + 0.5f), "%");

  if (featureSupport(Feature::ControlPromptHold) == Support::Unsupported) {
    log("Config:   ControlPrompt = unsupported on this game");
  } else if (!featureEnabled(Feature::ControlPromptHold)) {
    log("Config:   ControlPrompt = original animation");
  } else {
    log("Config:   ControlPrompt = ",
        duskConfigBool("Interface", "HideControlPrompt", false)
          ? "hidden" : "steady");
  }

  if (currentEngine() == Engine::Ktgl) {
    const int width = duskConfigInt("Rendering", "DisplayWidth", 0);
    const int height = duskConfigInt("Rendering", "DisplayHeight", 0);
    if (width > 0 && height > 0)
      log("Config:   DisplaySize = ", width, "x", height);
    else
      log("Config:   DisplaySize = automatic desktop size");
  } else {
    log("Config:   DisplaySize = managed by the game on this engine");
  }

  // Last, because it describes this log rather than the game. It also tells a
  // reader of a quiet log whether the diagnostic lines are absent or just off.
  log("Config:   VerboseLogging = ", verboseLogging() ? "on" : "off");
}

}  // namespace atfix
