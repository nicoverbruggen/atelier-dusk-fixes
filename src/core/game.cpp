// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <cstring>

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
    /* D3D11WriteProbe */ { "DUSK_D3D11_WRITE_PROBE", nullptr, nullptr },
    /* TargetCensus    */ { "DUSK_TARGET_CENSUS",     nullptr, nullptr },
    /* HighRes         */ { "DUSK_HIGHRES",           nullptr, nullptr },
    /* AtlasCache      */ { "DUSK_ATLAS_CACHE",       nullptr, nullptr },
    /* FieldEngineFix  */ { "DUSK_FIELD_ENGINE_FIX",  nullptr, nullptr },
    /* FieldStabilizer */ { "DUSK_FIELD_STABILIZER",  nullptr, nullptr },
    /* Smaa            */ { "DUSK_SMAA",     "Rendering", "SMAA" },
    /* Msaa            */ { "DUSK_MSAA",               nullptr, nullptr },
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
// text-rendering layer has no homolog of the hooked entry points
// (WORK_DOC.md "The engine triage").
//
// AtlasTrace is the same, and additionally implies AtlasStats: it records the
// raw atlas lock/unlock sequence of one steady-state frame and prints it, which
// is how the write-to-read pairing gets settled rather than guessed
// (WORK_DOC.md "Diagnostics"). It costs a mutex acquisition per lock, so it is
// strictly a bring-your-own-question switch.
//
// AtlasVerify is the correctness check for the cache, and is Ayesha-only and
// OptIn for a stronger reason than the others: it makes the game slow on purpose
// (a real atlas lock plus a ~1 MB comparison per verified read). It answers the
// one question a playthrough cannot, since a stale glyph in Japanese is not
// something a reader can reliably spot.
//
// AtlasCensus and D3D11WriteProbe are enumeration diagnostics rather than
// sampling ones: they exist to close the question of who can write a font atlas
// by listing every writer, which is a stronger result than any playthrough can
// give. Both are Ayesha-only and OptIn.
//
// AtlasCache is Ayesha-only and ships ON BY DEFAULT: it is the shipping fix.
// The pattern it addresses is measured, not assumed -- 2385 candidate locks onto
// 3 atlases per 248 ms drain, plus a per-frame steady-state drip -- and the
// measured effect is an 85% reduction in menu-build time at a 95.5% hit rate
// (WORK_DOC.md "Repeated font-atlas reads", "The lifetime is frame-scoped, and
// there are two problems", "Repeated font-atlas reads"). Like the field-jitter
// fix it has no ini key and no launcher control; `DUSK_ATLAS_CACHE=0` turns it
// off, which is what an A/B or a bug report wants.
//
// The two field-jitter halves are Ayesha-only and ship ON BY DEFAULT, as they
// do in Arland. They were held OptIn while nothing had been measured on Ayesha;
// that gap is now closed at both ends. The defect was quantified from a capture
// of the atelier's interior steps -- 12-18 px of vertical excursion while the
// character is horizontally at rest, a gravity-versus-threshold sawtooth rather
// than a bob -- and an in-game session with both switches on confirmed the fix
// (WORK_DOC.md "The field-jitter fix").
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
// Smaa is Ayesha-only and OptIn, and is the one shipping-quality feature here
// that is deliberately NOT on by default yet. The passes themselves are the
// Arland project's own, ported unchanged, so the antialiasing is not the
// experiment. What is missing is where to inject it: Arland runs SMAA on the
// scene target before the game composites its interface, and no equivalent
// boundary has been established for PhyreEngine. Until one is, this runs at
// Present over the finished frame, which antialiases the UI and its text along
// with the scene. That is a real visual cost and exactly the kind of trade-off
// a user should get to refuse, so it stays opt-in and keeps its switch.
//
// Escha & Logy and Shallie are Unsupported rather than OptIn only for want of
// evidence: the full-frame path needs no engine knowledge and would very likely
// work there unchanged. Nothing has been measured on KTGL, so nothing is
// claimed (WORK_DOC.md, "SMAA").
//
// Supersampling is OptIn on Ayesha and Unsupported on the KTGL games, and it is
// a rebuild rather than a repair: four implementations preceded it and none of
// them worked. A back-buffer redirect found nothing to attach to, because this
// engine never composites through the back buffer's render-target view.
// Enlarging the scene targets and letting the engine resample worked but gave
// four bilinear taps, and silently disabled MSAA because two places computed
// the scene size and disagreed. Owning the resample improved it marginally.
// Adding a once-per-frame latch to that pass produced a black scene, because
// the transition it latched on fires 5-22 times per frame and the first one is
// a post-processing bind rather than the composite.
//
// What ships now identifies the composite POSITIVELY -- the bind whose colour
// target is the swap-chain back buffer, which is a runtime fact and exact --
// and substitutes a box-filtered display-sized copy of the scene in the
// argument array of the one PSSetShaderResources call that composite makes.
// Nothing is latched across calls and nothing at all happens at Present. See
// supersample.h and WORK_DOC.md.
//
// OptIn rather than on by default, and it can never be anything else: 200% is
// four times the shaded pixels, measured at 70% GPU on a 7900 XTX in the game's
// opening interior, which is close to the lightest scene there is. Combined
// with 4x MSAA that is sixteen geometry samples per displayed pixel.
//
// Its key is the one in this table that featureEnabled must not be asked about
// -- see the note on the Descriptor row.
//
// TargetCensus is the one diagnostic that is NOT Ayesha-only. It reads nothing
// but the D3D11 resources the game creates, so it needs no mapped address and
// no engine knowledge, and the question it answers -- does this game size its
// internal render targets from the resolution it was asked for, or does it pin
// some of them to 1080p the way the old-Arland renderer does -- is open for all
// three games (WORK_DOC.md "Ayesha's resolution path"). It stays OptIn
// everywhere: it hooks CreateTexture2D, which no shipping configuration should.
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
// Escha & Logy and Shallie are Unsupported rather than OptIn because their
// renderer has not been censused at all; enabling this there would apply a rule
// derived from a different engine.
constexpr Support kMatrix[3][static_cast<int>(Feature::Count)] = {
  //                Stats Trace Verfy Censu Probe Targt HiRes Cache Field Stab Smaa Msaa Ssaa Aniso
  /* Ayesha  */   {   O,    O,    O,    O,    O,    O,    X,    X,    X,    X,   X,   O,   O,   X },
  /* Escha   */   {   U,    U,    U,    U,    U,    O,    U,    U,    U,    U,   U,   U,   U,   O },
  /* Shallie */   {   U,    U,    U,    U,    U,    O,    U,    U,    U,    U,   U,   U,   U,   O },
};

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
