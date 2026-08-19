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
// every fix this mod ships: the font-atlas read cache, the field-movement
// corrections, and the high-resolution correction. Their environment switches
// remain so an A/B or a bug report can stand one down for a session.
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
    /* AtlasVerify     */ { "DUSK_ATLAS_VERIFY",      nullptr, nullptr },
    /* TargetCensus    */ { "DUSK_TARGET_CENSUS",     nullptr, nullptr },
    /* HighResRendering */ { "DUSK_HIGHRES",           nullptr, nullptr },
    /* AtlasCache      */ { "DUSK_ATLAS_CACHE",       nullptr, nullptr },
    /* FieldEngineFix  */ { "DUSK_FIELD_ENGINE_FIX",  nullptr, nullptr },
    /* Smaa            */ { "DUSK_SMAA",     "Rendering", "SMAA" },
    // Env-only DESPITE having an ini key, which is the one exception in this
    // table and needs stating. `[Rendering] Supersampling` is an INT
    // percentage, and featureEnabled's boolean path would seed the literal
    // `false` into it the first time anything asked whether the feature was on.
    // ssaaPercent() owns that key and parses it as the integer it is; the
    // capability matrix still owns whether the running game supports the
    // feature at all, which is the only question asked of this row.
    /* Supersampling   */ { "DUSK_SSAA",               nullptr, nullptr },
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
    /* PadRescanBackoff */
                          { "DUSK_PAD_RESCAN",        nullptr, nullptr },
    /* SynthesisAnimationRate */
                          { "DUSK_SYNTH_RATE",        nullptr, nullptr },
    // A valued knob rather than a switch, so no ini key here: shadow_res.cpp
    // reads [Rendering] ShadowMultiplier itself, and 1 means off. Giving the
    // row a key would have featureEnabled seed the literal "false" into an
    // integer setting, which is the trap Supersampling documents above.
    //
    // Its matrix cell is DOCUMENTATION ONLY, as the Arland project's is:
    // nothing resolves behaviour through featureEnabled for this row, so the
    // cell says which games have the feature and the ini says whether it runs.
    // Ayesha's cell reads OnByDefault because shadowMapResolution() reads a
    // missing key as 2, the same reasoning and the same value as Arland.
    /* ShadowMultiplier */
                          { "DUSK_SHADOW_MULTIPLIER", nullptr, nullptr },
    // No ini key on purpose: this is a defect correction that is simply on, so
    // a key would document a decision nobody has to make. The env override
    // remains for a diagnostic run.
    /* FieldCharacterPull */
                          { "DUSK_CHARACTER_PULL", nullptr, nullptr },
    // Also keyless, and for the same reason: a defect correction rather than a
    // setting. DUSK_TALK_ANCHOR=0 turns it off for a diagnostic run.
    /* TalkAnchorHold */  { "DUSK_TALK_ANCHOR", nullptr, nullptr },
    // Keyless for the third time, and the reason is the same: a display that is
    // not 16:9 showing a stretched picture is a defect, not a preference.
    // DUSK_LETTERBOX=0 turns it off, which is the run that tells a doubled
    // letterbox apart from an absent one.
    /* Letterbox */       { "DUSK_LETTERBOX", nullptr, nullptr },
    // Keyed, and on the Debug page rather than among the settings, which is
    // the one place this table's rule about corrections bends. It is a defect
    // correction and so not a preference -- but it changes how a character
    // stands on terrain, which is the kind of thing a player might want to see
    // without for a moment, and the Arland window already carries its field
    // switches the same way. The key is absent at the default and written only
    // when someone turns the fix off.
    // Keyless, and valued rather than boolean: the switch also carries the
    // millisecond count, so worker_idle_sleep.cpp reads it directly and this
    // cell only says which games have the worker at all.
    /* WorkerIdleSleep */
                          { "DUSK_WORKER_IDLE_SLEEP", nullptr, nullptr },
    /* FieldSlopeHold */  { "DUSK_SLOPE_HOLD", "Debug", "SlopeHold" },
    // Same shape as WorkerIdleSleep above; the record is in present_throttle.h.
    /* MinimizedThrottle */
                          { "DUSK_MINIMIZED_THROTTLE", nullptr, nullptr },
  };
  return table[static_cast<int>(f)];
}

constexpr Support U = Support::Unsupported;
constexpr Support O = Support::OptIn;
constexpr Support X = Support::OnByDefault;

// The capability matrix. Rows are Ayesha / Escha & Logy / Shallie, columns
// follow the Feature enum, and a cell is X on by default, O opt-in, U
// unsupported. KEEP IN SYNC with README.md's feature table.
//
// A cell says whether this game can have the feature and what it defaults to,
// and nothing else. Why it defaults that way belongs beside the code that
// answers it: engines/phyre/atlas_fix.h, core/supersample.h, core/highres.h,
// engines/phyre/field_physics.h, and so on.
//
// A feature with no ini key is stood down for one session with its environment
// switch, DUSK_<FEATURE>=0, which is what an A/B or a bug report wants. See the
// note on Descriptor above for why a correction does not become a setting.
//
// One row per Feature. Each row names its Feature, and each cell names the game
// it answers for -- and the three games are DISTINCT TYPES, so putting Escha's
// answer in Shallie's column does not compile.
//
// The previous form was three bare positional arrays under a hand-maintained
// list of abbreviated column names. Nothing checked that list against the
// columns, and by the time this replaced it, it carried twenty names over
// twenty-one columns.
struct AyeshaCell  { Support value; };
struct EschaCell   { Support value; };
struct ShallieCell { Support value; };

constexpr AyeshaCell  Ayesha(Support s)  { return { s }; }
constexpr EschaCell   Escha(Support s)   { return { s }; }
constexpr ShallieCell Shallie(Support s) { return { s }; }

struct SupportRow {
  Feature feature;
  AyeshaCell ayesha;
  EschaCell escha;
  ShallieCell shallie;
};

constexpr SupportRow kSupport[] = {
  { Feature::AtlasVerify,            Ayesha(O), Escha(U), Shallie(U) },
  { Feature::TargetCensus,           Ayesha(O), Escha(O), Shallie(O) },
  { Feature::HighResRendering,       Ayesha(X), Escha(U), Shallie(U) },
  { Feature::AtlasCache,             Ayesha(X), Escha(U), Shallie(U) },
  { Feature::FieldEngineFix,         Ayesha(X), Escha(U), Shallie(U) },
  { Feature::Smaa,                   Ayesha(X), Escha(X), Shallie(X) },
  { Feature::Supersampling,          Ayesha(O), Escha(O), Shallie(O) },
  { Feature::WorldMapCursor,         Ayesha(X), Escha(X), Shallie(U) },
  { Feature::SkipStartupLogos,       Ayesha(O), Escha(O), Shallie(O) },
  { Feature::SkipIntroMovie,         Ayesha(O), Escha(O), Shallie(O) },
  { Feature::LoadingTextTypo,        Ayesha(U), Escha(X), Shallie(X) },
  { Feature::SystemSaveGuard,        Ayesha(U), Escha(X), Shallie(X) },
  { Feature::PadRescanBackoff,       Ayesha(X), Escha(X), Shallie(X) },
  { Feature::SynthesisAnimationRate, Ayesha(X), Escha(X), Shallie(X) },
  { Feature::ShadowMultiplier,       Ayesha(X), Escha(U), Shallie(U) },
  { Feature::FieldCharacterPull,     Ayesha(X), Escha(U), Shallie(U) },
  { Feature::TalkAnchorHold,         Ayesha(X), Escha(U), Shallie(U) },
  { Feature::Letterbox,              Ayesha(U), Escha(X), Shallie(X) },
  { Feature::WorkerIdleSleep,        Ayesha(U), Escha(U), Shallie(U) },
  { Feature::FieldSlopeHold,         Ayesha(U), Escha(X), Shallie(U) },
  { Feature::MinimizedThrottle,      Ayesha(U), Escha(X), Shallie(X) },
};

static_assert(std::size(kSupport) == static_cast<std::size_t>(Feature::Count),
              "the support table is not one row per Feature");

// Every row must sit at its own Feature's index. That is what lets the lookup
// index directly, and what catches a row inserted or moved without its
// neighbours -- the failure the old positional form could not see.
constexpr bool supportRowsInEnumOrder() {
  for (std::size_t i = 0; i < std::size(kSupport); ++i) {
    if (static_cast<std::size_t>(kSupport[i].feature) != i)
      return false;
  }
  return true;
}
static_assert(supportRowsInEnumOrder(),
              "a support row is not at its own Feature's index");

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
  const SupportRow& row = kSupport[static_cast<std::size_t>(f)];
  switch (currentTitle()) {
    case Title::Ayesha:  return row.ayesha.value;
    case Title::Escha:   return row.escha.value;
    case Title::Shallie: return row.shallie.value;
    default:             return Support::Unsupported;
  }
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
