// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

// Per-game capability layer, mirroring the Arland project's game.h. This header
// plus the matrix in game.cpp are the single source of truth for which
// enhancements each Dusk game receives and whether they are on by default.
// Resolved independently of whether any hook installs, so other layers can gate
// on the title even when the game-code hooks are off.
namespace atfix {

// Which Dusk game we run in, detected from the executable name. Unlike the
// Arland games (A11R/A12V/A13V prefixes) the Dusk executables are named after
// the title, and each ships an English and a multilingual build:
//   Atelier_Ayesha_EN.exe / Atelier_Ayesha.exe
//   Atelier_Escha_and_Logy_EN.exe / Atelier_Escha_and_Logy.exe
//   Atelier_Shallie_EN.exe / Atelier_Shallie.exe
enum class Title : uint8_t { Unknown, Ayesha, Escha, Shallie };
Title currentTitle();
const char* titleName(Title t);

// Enhancements the mod can apply. Order must match the matrix columns and the
// descriptor rows in game.cpp.
//
// All four are implemented; all four are OptIn, because nothing in this project
// has been validated in-game yet.
enum class Feature : uint8_t {
  AtlasStats,       // Ayesha font-atlas diagnostic counters (measurement only)
  AtlasCache,       // Ayesha atlas read caching (frame-scoped)
  FieldEngineFix,   // Ayesha high-refresh field jitter: rescale the move threshold
  FieldStabilizer,  // Ayesha high-refresh field jitter: hold the character at rest
  Count,
};

// How a feature relates to the current game.
enum class Support : uint8_t {
  Unsupported,   // inapplicable here; env cannot force it on
  OptIn,         // available but off unless the user enables it
  OnByDefault,   // on unless the user disables it
};

// Per-game support/default for a feature (the capability matrix).
Support featureSupport(Feature f);

// Resolved on/off for the current game. Precedence: environment override, then
// the matrix default. Unsupported is a hard off that the environment cannot
// turn on.
bool featureEnabled(Feature f);

}  // namespace atfix
