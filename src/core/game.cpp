// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "config.h"
#include "game.h"

namespace atfix {

namespace {

const char* baseName(const char* path) {
  const char* back = std::strrchr(path, '\\');
  const char* forward = std::strrchr(path, '/');
  const char* sep = back > forward ? back : forward;
  return sep ? sep + 1 : path;
}

// Matched on a prefix so both the English and the multilingual build of each
// game resolve to the same title (Atelier_Ayesha_EN.exe and
// Atelier_Ayesha.exe). Escha must be tested before any shorter prefix that
// could shadow it; none currently can, but keep the checks disjoint.
Title detectTitle() {
  HMODULE module = GetModuleHandleW(nullptr);
  char path[MAX_PATH] = {};
  if (!module || !GetModuleFileNameA(module, path, sizeof(path)))
    return Title::Unknown;
  const char* name = baseName(path);
  if (!_strnicmp(name, "Atelier_Ayesha", 14)) return Title::Ayesha;
  if (!_strnicmp(name, "Atelier_Escha_and_Logy", 22)) return Title::Escha;
  if (!_strnicmp(name, "Atelier_Shallie", 15)) return Title::Shallie;
  return Title::Unknown;
}

// Where a feature's overrides live. `env` is always present; `section`/`key`
// are set only for the options a user is meant to configure.
//
// That asymmetry is the Arland rule carried over: dusk-fix.ini is the
// user-facing surface, and an environment switch on its own is not one. Giving
// a diagnostic an ini key would eventually get it turned on by someone
// following a forum post, and these diagnostics are slow on purpose --
// AtlasVerify adds a real atlas lock and a ~1 MB comparison per verified read.
//
// The shipping fixes are env-only for the opposite reason: not because they are
// dangerous, but because they are not choices. A fix that is simply correct,
// on by default, and has no configuration a user could reason about is not a
// setting, and a key in the file is an invitation to turn it off. That covers
// every fix this mod ships: the font-atlas read cache, both field-jitter halves
// (further coupled -- the stabilizer needs the rescale), and the
// high-resolution correction. Their environment switches remain so an A/B or a
// bug report can stand one down for a session.
//
// The high-resolution correction is the one that looks like a counter-example
// and is not. Rendering at 4K instead of 1080p does cost real performance, so
// there is a decision here -- but the player already makes it when they pick a
// resolution. The fix only makes the resolution they chose the one actually
// rendered. A separate key asking whether the chosen resolution should be used
// is not a preference, it is the same preference asked twice.
struct Descriptor {
  const char* env;
  const char* section;
  const char* key;
};

const Descriptor& descriptor(Feature f) {
  static const Descriptor table[static_cast<int>(Feature::Count)] = {
    /* AtlasStats      */ { "DUSK_ATLAS_STATS",       nullptr, nullptr },
    /* AtlasTrace      */ { "DUSK_ATLAS_TRACE",       nullptr, nullptr },
    /* AtlasVerify     */ { "DUSK_ATLAS_VERIFY",      nullptr, nullptr },
    /* AtlasCensus     */ { "DUSK_ATLAS_CENSUS",      nullptr, nullptr },
    /* TargetCensus    */ { "DUSK_TARGET_CENSUS",     nullptr, nullptr },
    /* HighRes         */ { "DUSK_HIGHRES",           nullptr, nullptr },
    /* AtlasCache      */ { "DUSK_ATLAS_CACHE",       nullptr, nullptr },
    /* FieldEngineFix  */ { "DUSK_FIELD_ENGINE_FIX",  nullptr, nullptr },
    /* FieldStabilizer */ { "DUSK_FIELD_STABILIZER",  nullptr, nullptr },
    /* Smaa            */ { "DUSK_SMAA",     "Rendering", "SMAA" },
    // Env-only DESPITE having an ini key, which is the one exception in this
    // table and needs stating. `[Rendering] Supersampling` is an INT
    // percentage, and featureEnabled's boolean path would seed the literal
    // `false` into it the first time anything asked whether the feature was on.
    // ssaaPercent() owns that key and parses it as the integer it is; the
    // capability matrix still owns whether the running game supports the
    // feature at all, which is the only question asked of this row.
    /* Supersampling   */ { "DUSK_SSAA",               nullptr, nullptr },
    /* AnisotropicFiltering */
                          { "DUSK_ANISO", "Rendering", "AnisotropicFiltering" },
    /* WorldMapCursor  */ { "DUSK_WORLDMAP",          nullptr, nullptr },
    // The two startup skips get ini keys where the fixes above do not, and
    // the difference is the house rule rather than an inconsistency: these
    // suppress behaviour the game shipped deliberately, so they are
    // preferences. The key names match the Arland mod's exactly, because the
    // two launchers are meant to present the same surface.
    /* SkipStartupLogos */
                          { "DUSK_SKIP_LOGOS",   "Startup", "SkipLogos" },
    /* SkipIntroMovie  */ { "DUSK_SKIP_INTRO_MOVIE",
                                                 "Startup", "SkipIntroMovie" },
    /* LoadingTextTypo */ { "DUSK_LOADING_TEXT",      nullptr, nullptr },
    /* SystemSaveGuard */ { "DUSK_SYSTEM_SAVE",        nullptr, nullptr },
    /* ControlPromptHold */
                          { "DUSK_CONTROL_PROMPT", "Interface", "SteadyControlPrompt" },
    /* PadRescanBackoff */
                          { "DUSK_PAD_RESCAN",        nullptr, nullptr },
    /* SynthesisAnimationRate */
                          { "DUSK_SYNTH_RATE",        nullptr, nullptr },
  };
  return table[static_cast<int>(f)];
}

constexpr Support U = Support::Unsupported;
constexpr Support O = Support::OptIn;
constexpr Support X = Support::OnByDefault;

// The capability matrix. Rows are Ayesha / Escha & Logy / Shallie, columns
// follow the Feature enum. KEEP IN SYNC with README.md's feature table.
//
// AtlasStats is Ayesha-only and OptIn: it is a diagnostic, so it must never be
// on by default, and it is meaningless on the other two games because their
// text-rendering layer has no homolog of the hooked entry points.
//
// AtlasTrace is the same, and additionally implies AtlasStats: it records the
// raw atlas lock/unlock sequence of one steady-state frame and prints it, which
// is how the write-to-read pairing gets settled rather than guessed. It costs
// a mutex acquisition per lock, so it is strictly a bring-your-own-question
// switch.
//
// AtlasVerify is the correctness check for the cache, and is Ayesha-only and
// OptIn for a stronger reason than the others: it makes the game slow on purpose
// (a real atlas lock plus a ~1 MB comparison per verified read). It answers the
// one question a playthrough cannot, since a stale glyph in Japanese is not
// something a reader can reliably spot.
//
// AtlasCensus is an enumeration diagnostic rather than a sampling one: it
// closes the question of who can write a font atlas by listing every engine
// writer, which is a stronger result than any playthrough can give. It is
// Ayesha-only and OptIn. The spent D3D11-level probe was removed after it
// answered its one investigation question; a diagnostic must not remain as a
// second permanent owner of the device-context vtables.
//
// AtlasCache is Ayesha-only and ships ON BY DEFAULT: it is the shipping fix.
// The pattern it addresses is measured, not assumed -- 2385 candidate locks onto
// 3 atlases per 248 ms drain, plus a per-frame steady-state drip -- and the
// measured effect is an 85% reduction in menu-build time at a 95.5% hit rate
// (see engines/phyre/atlas_fix.h). Like the field-jitter fix it has no ini
// key and no launcher control; `DUSK_ATLAS_CACHE=0` turns it off, which is what
// an A/B or a bug report wants.
//
// The two field-jitter halves are Ayesha-only and ship ON BY DEFAULT, as they
// do in Arland. They were held OptIn while nothing had been measured on Ayesha;
// that gap is now closed at both ends. The defect was quantified from a capture
// of the atelier's interior steps -- 12-18 px of vertical excursion while the
// character is horizontally at rest, a gravity-versus-threshold sawtooth rather
// than a bob -- and an in-game session with both switches on confirmed the fix
// (see engines/phyre/field_physics.h).
//
// They are one feature in two keys, not two features. The stabilizer needs the
// rescale it builds on and refuses to run without it (installFieldPhysics), so
// promoting the rescale alone would have left the resting case unfixed, and
// promoting the stabilizer alone does nothing at all. That is also why neither
// appears in the launcher and neither has an ini key: two coupled switches for
// a single fix that is simply correct is not a setting. `DUSK_FIELD_ENGINE_FIX=0`
// stands the whole thing down for one session, which is what an A/B or a bug
// report wants and is the only override that exists.
//
// The offsets the stabilizer writes through were the last thing here resting on
// carry-over rather than evidence. They have since been confirmed against both
// Ayesha builds -- see the comment on the offset constants in field_physics.cpp
// -- so nothing about this fix is now inherited on trust.
//
// Smaa is available in all three games and ON BY DEFAULT in all three, because
// all three have a pre-UI injection point. smaa.cpp runs there on the scene
// target before the game composites its interface, so menus and text are left
// alone, and that is what makes on-by-default defensible. The passes themselves
// are the Arland project's own, ported unchanged, so the antialiasing is never
// the experiment; the injection point is.
//
// Ayesha has always had a scene-target test. Escha & Logy and Shallie got one
// in src/engines/ktgl/scene_target.cpp -- the first draw into the surface the
// interface is about to be drawn into. Both cells were OptIn for exactly as
// long as the full-frame path was the only one those two games had, which
// stopped being true when that test landed.
//
// Nothing engine-specific is involved in the fallback, and that is the whole
// reason these cells can change without new addresses: smaaApply takes the swap
// chain's back buffer, creates a render-target view over it and runs the
// passes. No scene test, no mapped address, no dependency on the
// high-resolution fix. The three gates in smaaApply that stand the Present path
// down -- the per-frame latch, the pre-UI-proven latch, and g_broken -- are all
// driven by the pre-UI path.
//
// The fallback is still reached, for the frames before the scene test accepts a
// target. Measured 2026-08-09: Escha & Logy logged the full-frame line at
// 621 ms and the pre-UI line at 3727 ms; Shallie 1695 ms and 3032 ms. So boot
// and the logos are antialiased whole and everything after them is not, which
// is a bounded cost rather than the standing one an earlier version of this
// comment described.
//
// The log says which one ran. "SMAA: pre-UI active" is the wanted path;
// "SMAA: running at Present over the finished frame -- the interface is
// antialiased too" is the fallback. Those two lines exist because "SMAA is on"
// and "SMAA is on in the good place" are different facts.
//
// Supersampling is OptIn on all three games. On Ayesha it is a rebuild rather
// than a repair: four implementations preceded it and none of them worked. A
// back-buffer redirect found nothing to attach to, because this engine never
// composites through the back buffer's render-target view.
// Enlarging the scene targets and letting the engine resample worked but gave
// four bilinear taps. Owning the resample improved it marginally.
// Adding a once-per-frame latch to that pass produced a black scene, because
// the transition it latched on fires 5-22 times per frame and the first one is
// a post-processing bind rather than the composite.
//
// What ships now identifies the composite POSITIVELY -- the bind whose colour
// target is the swap-chain back buffer, which is a runtime fact and exact --
// and substitutes a box-filtered display-sized copy of the scene in the
// argument array of the one PSSetShaderResources call that composite makes.
// Nothing is latched across calls and nothing at all happens at Present. See
// supersample.h.
//
// OptIn rather than on by default, and it can never be anything else: 200% is
// four times the shaded pixels, measured at 70% GPU on a 7900 XTX in the game's
// opening interior, which is close to the lightest scene there is.
//
// Its key is the one in this table that featureEnabled must not be asked about
// -- see the note on the Descriptor row.
//
// Escha & Logy reaches the same feature by the opposite route, and its cell is
// OptIn for the same cost reason. Ayesha pins its scene targets, so the mod
// enlarges them and owns the resolve. KTGL sizes every full-frame target from
// its own Setting.ini, so the launcher writes that file at base x factor and
// the mod only has to stop the swap chain following it up: it clamps the back
// buffer to the base and resolves the oversized scene into it at the
// composite's own sample. A run confirmed the engine half with no
// code at all -- Setting.ini at 5120x2880 on a 1440p panel produced every
// full-frame target at 5120x2880, blur pyramid chaining down from it.
//
// Shallie gets the same cell, and the caution that kept it Unsupported for a
// day turned out to be about a design that was abandoned. It has roughly twice
// as many places re-reading the resolution out of the ini as Escha does, which
// would have mattered to the settings-reader hook that was never built: that
// one wrote the in-memory settings object and left the file holding the base,
// so every re-reader would have disagreed with it. THE SHIPPED DESIGN WRITES
// THE FILE. Every re-reader gets the multiplied number, because that is what is
// on disk, and there is nothing to disagree with.
//
// One residual is real rather than theoretical, and it is Shallie-only: its
// `0x5881a0` takes a width from the bound surface and a height from a fresh ini
// read. The mod clamps that surface, so those two now come from different
// worlds and the result would be a target with the wrong aspect. It is one
// function, the failure would be visible rather than silent, and enabling the
// row is what puts a run in front of it.
//
// WorldMapCursor is Ayesha-only and ships ON BY DEFAULT, on the same reasoning
// as the field-jitter fix it is a sibling of: a cursor that crosses the map
// three times too fast at 200 Hz is not a preference anyone would choose, it is
// the game behaving differently from the way it was built to. The Arland
// project reached the same conclusion for Totori and Meruru.
//
// `DUSK_WORLDMAP=0` stands it down for a comparison, and it has no ini key for
// the reason given on Descriptor: a correction is not a setting.
//
// Escha & Logy ships it on too, for the same reason and on the same evidence
// shape: its cursor mover scales the stick vector by a constant per frame and
// Shallie's multiplies by the frame delta, read from the bytes of both. See
// engines/ktgl/worldmap_fix.h.
//
// SHALLIE STAYS UNSUPPORTED, and that is a finding rather than a gap: its mover
// is already correct, so hooking it would rescale a rate that does not need it.
// Its d-pad branches do still overwrite the scaled value with a flat constant,
// which is a separate defect in a shared menu object and is not this row.
//
// TargetCensus is the one diagnostic that is NOT Ayesha-only. It reads nothing
// but the D3D11 resources the game creates, so it needs no mapped address and
// no engine knowledge, and the question it answers -- does this game size its
// internal render targets from the resolution it was asked for, or does it pin
// some of them to 1080p the way the old-Arland renderer does -- is open for all
// three games. It stays OptIn everywhere: it hooks CreateTexture2D, which no
// shipping configuration should.
//
// HighResRendering is Ayesha-only and ships ON BY DEFAULT, applied the same way
// the Arland project applies its own correction: automatically, whenever the
// resolution the player selected is higher than 1080p. Nothing here asks the
// user to know that the engine pins its internal targets. The trigger is not in
// this matrix at all -- highres.cpp resizes only while the learned main render
// size exceeds the pinned 1920x1080 (kPinnedWidth/kPinnedHeight), which is
// Arland's `mainWidth > 1920 && mainHeight > 1080` rule carried over. At 1080p
// and below every hook stays installed and every target passes through, so
// being on by default costs a player at 1080p nothing.
//
// `DUSK_HIGHRES=0` stands it down for a session, which matters more here than
// for the other fixes: the failure mode is a visibly wrong picture (a viewport
// or a target that did not move with the rest) rather than a silent regression,
// so a player who hits one needs a way to confirm what they are looking at. It
// has no ini key and no launcher control, for the reason given on Descriptor.
//
// The defect is measured and the mechanism is TellowKrinkle's, proven on this
// engine family and refined in the Arland project, so this is not an experiment
// in the way the field-jitter switches are.
//
// Escha & Logy and Shallie are Unsupported, and the reason is stronger than
// "their renderer has not been censused". They do not have the defect: both
// size the swap chain, the back buffer, the depth target and every full-frame
// intermediate from the two Setting.ini values, and a byte-exhaustive census of
// all four KTGL executables found no 1920/1080 pair outside the UI canvas
// initializer. There is nothing pinned to correct.
//
// Enabling it there would be worse than useless at one specific resolution.
// isPinnedFullTarget matches exactly 1920x1080 and isPinnedBlurTarget exactly
// 960x540. On an engine that derives its sizes, a player running 3840x2160 has
// half-frame intermediates at exactly 1920x1080 and quarter-frame ones at
// exactly 960x540 -- so both rules would match surfaces that are already the
// right size and double them. The rules are keyed on absolute sizes, which is
// only meaningful in an engine that pins them; in an engine that derives them
// the same numbers are a coincidence of the chosen resolution.
//
// LoadingTextTypo is the mirror image of every row above it: the two KTGL games
// get it and Ayesha does not, because Ayesha does not have the defect. The
// literal "Loadning system data." is in all four Escha & Logy and Shallie
// executables and in neither Ayesha build (loading_text_fix.h), so Ayesha's U
// here is a fact about the binary rather than a gap in the evidence.
//
// It ships ON BY DEFAULT, and it is the least arguable row in this table: a
// misspelling on the first screen of the game is not a preference, and the
// correction costs one 22-byte store at startup, runs no code afterwards, and
// cannot change anything a player would want back. `DUSK_LOADING_TEXT=0` stands
// it down for a comparison, and it has no ini key for the reason given on
// Descriptor.
//
// It is also the one row here that needs no engine knowledge whatsoever, which
// is why it can ship for KTGL while everything else in this table cannot: it
// rewrites a string literal in the mapped image and hooks nothing.
//
// NOTE on the WorldMapCursor column. It was absent from these rows at first, so
// the array's trailing elements were value-initialized and every game silently
// read Unsupported for it -- contradicting the paragraph above, which says
// Ayesha ships it on by default. It is spelled out now, and the address pack it
// was waiting on has since been derived for both Ayesha builds, so the column
// and the code finally agree.
//
// The rows are three separate arrays of DEDUCED extent rather than one
// `[3][Count]` block, which is the whole reason that column could go missing
// unnoticed: with the width declared, a short row is not an error, the trailing
// entries are value-initialized, and Unsupported happens to be the zero value --
// so an incomplete row reads as a deliberate "this game does not get it". Let
// the extent come from the initializer instead and each static_assert below
// fails loudly the next time a Feature is added without extending every row.
//                               Stats Trace Verfy Censu Targt HiRes Cache Field Stabl Smaa  Ssaa  Aniso WMap  Logo  Movi  Typo  SysSv Promt PadRe Synth
constexpr Support kAyesha[]  = { O,    O,    O,    O,    O,    X,    X,    X,    X,    X,    O,    X,    X,    O,    O,    U,    U,    U,    X,    U };
constexpr Support kEscha[]   = { U,    U,    U,    U,    O,    U,    U,    U,    U,    X,    O,    O,    X,    O,    O,    X,    X,    U,    X,    X };
constexpr Support kShallie[] = { U,    U,    U,    U,    O,    U,    U,    U,    U,    X,    O,    O,    U,    O,    O,    X,    X,    O,    X,    X };

constexpr std::size_t kColumns = static_cast<std::size_t>(Feature::Count);
static_assert(std::size(kAyesha) == kColumns,  "Ayesha row is not one entry per Feature");
static_assert(std::size(kEscha) == kColumns,   "Escha row is not one entry per Feature");
static_assert(std::size(kShallie) == kColumns, "Shallie row is not one entry per Feature");

constexpr const Support* kMatrix[3] = { kAyesha, kEscha, kShallie };

int titleRow(Title t) {
  switch (t) {
    case Title::Ayesha:  return 0;
    case Title::Escha:   return 1;
    case Title::Shallie: return 2;
    default: return -1;
  }
}

}  // namespace

Title currentTitle() {
  static const Title title = detectTitle();
  return title;
}

Engine currentEngine() {
  switch (currentTitle()) {
    case Title::Ayesha:  return Engine::Phyre;
    case Title::Escha:
    case Title::Shallie: return Engine::Ktgl;
    default: return Engine::Unknown;
  }
}

const char* engineName(Engine e) {
  switch (e) {
    case Engine::Phyre: return "Phyre";
    case Engine::Ktgl:  return "KTGL";
    default: return "Unknown";
  }
}

const char* titleName(Title t) {
  switch (t) {
    case Title::Ayesha:  return "Ayesha";
    case Title::Escha:   return "Escha & Logy";
    case Title::Shallie: return "Shallie";
    default: return "Unknown";
  }
}

Support featureSupport(Feature f) {
  const int row = titleRow(currentTitle());
  if (row < 0)
    return Support::Unsupported;
  return kMatrix[row][static_cast<int>(f)];
}

bool featureEnabled(Feature f) {
  const Support support = featureSupport(f);
  if (support == Support::Unsupported)
    return false;
  const Descriptor& d = descriptor(f);
  // Environment first, so a diagnostic run overrides a persisted setting
  // without editing the user's file.
  if (d.env) {
    if (const char* v = std::getenv(d.env))
      return v[0] != '0';
  }
  const bool def = support == Support::OnByDefault;
  // Seeded here rather than in configPath()'s first-use block so the key is
  // written with the default for the game it is running in, and so a game that
  // supports the feature not at all never grows a key for it: the Unsupported
  // hard-off above returns before this point.
  if (d.section && d.key)
    return duskConfigBool(d.section, d.key, def);
  return def;
}

}  // namespace atfix
