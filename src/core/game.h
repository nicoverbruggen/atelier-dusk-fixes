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

// Which rendering/text engine the current title runs on. This is the axis the
// source tree is split along, because the two share almost nothing that a fix
// can reach: Ayesha is the PhyreEngine-derived, old-MSVC-CRT build whose
// font-atlas and text-rendering path is the same code as the Arland games',
// while Escha & Logy and Shallie are UCRT builds on LTGL/KTGL whose text layer
// has no homolog of it at all.
//
// One DLL still covers all three: the engine is resolved from the executable at
// startup and only that engine's module installs anything. See `core/engine.h`.
enum class Engine : uint8_t { Unknown, Phyre, Ktgl };
Engine currentEngine();
const char* engineName(Engine e);

// Enhancements the mod can apply. Order must match the matrix columns and the
// descriptor rows in game.cpp.
//
// AtlasCache, HighResRendering, and both field-jitter halves ship on by
// default for Ayesha. The first six rows are diagnostics. Everything from
// LoadingTextTypo down belongs to the two KTGL games, and only TargetCensus,
// AnisotropicFiltering and PadRescanBackoff apply to all three.
enum class Feature : uint8_t {
  AtlasStats,       // Ayesha font-atlas diagnostic counters (measurement only)
  AtlasTrace,       // Ayesha font-atlas lock/unlock sequence trace (one frame)
  AtlasVerify,      // Ayesha font-atlas snapshot-vs-real comparison (slow)
  AtlasCensus,      // Ayesha font-atlas writer census (enumerates every caller)
  D3D11WriteProbe,  // Ayesha D3D11-level writes to a font atlas, if any exist
  TargetCensus,     // any game: sizes of the render/depth targets it creates
  HighResRendering, // Ayesha: scene targets follow the resolution, not 1080p
  AtlasCache,       // Ayesha atlas read caching (frame-scoped)
  FieldEngineFix,   // Ayesha high-refresh field jitter: rescale the move threshold
  FieldStabilizer,  // Ayesha high-refresh field jitter: hold the character at rest
  Smaa,             // Ayesha: SMAA post-process antialiasing
  Supersampling,    // Ayesha: render above the display size, downscale to it
  AnisotropicFiltering, // any game: upgrade trilinear samplers (sampler.h)
  WorldMapCursor,   // Ayesha: travel-map cursor moves per second, not per frame
  SkipStartupLogos, // Ayesha: skip the publisher/developer logos shown at boot
  SkipIntroMovie,   // Ayesha: skip the movies played on the way to the title
  LoadingTextTypo,  // Escha & Logy, Shallie: "Loadning" -> "Loading"
  SystemSaveGuard,  // Escha & Logy, Shallie: refuse to overwrite a failed load
  ControlPromptHold,// Shallie: the control-hint panel stops replaying its slide
  PadRescanBackoff, // any game: rate-limit the controller rescan (unmeasured)
  SynthesisAnimationRate, // Escha & Logy, Shallie: synthesis cards tick at 59.94 Hz
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
