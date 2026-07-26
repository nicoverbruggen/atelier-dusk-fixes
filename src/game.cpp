// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <cstring>

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

// Where a feature's override lives. A null env means the feature has no
// environment override. No ini layer exists yet -- the Dusk project has no
// config file, so the Arland Descriptor's section/key fields are omitted rather
// than left dangling.
struct Descriptor {
  const char* env;
};

const Descriptor& descriptor(Feature f) {
  static const Descriptor table[static_cast<int>(Feature::Count)] = {
    /* AtlasStats      */ { "DUSK_ATLAS_STATS" },
    /* AtlasCache      */ { "DUSK_ATLAS_CACHE" },
    /* FieldEngineFix  */ { "DUSK_FIELD_ENGINE_FIX" },
    /* FieldStabilizer */ { "DUSK_FIELD_STABILIZER" },
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
// (TECHNICAL.md 1.3).
//
// AtlasCache is Ayesha-only and ships ON BY DEFAULT: it is the shipping fix.
// The pattern it addresses is measured, not assumed -- 2385 candidate locks onto
// 3 atlases per 248 ms drain, plus a per-frame steady-state drip -- and the
// measured effect is an 85% reduction in menu-build time at a 95.5% hit rate
// (TECHNICAL.md §2.1, §2.3, §4.1). `DUSK_ATLAS_CACHE=0` turns it off, which is
// what an A/B or a bug report wants.
//
// The two field-jitter halves are Ayesha-only and stay OptIn. They are ported
// from Arland (where both are on by default) but nothing has been measured on
// Ayesha: no run has confirmed the high-refresh jitter is present here. The
// stabilizer additionally writes into the controller object at offsets not yet
// confirmed for this build -- see the comment on stabilizerEnabled in
// field_physics.cpp. They must not be promoted alongside the cache.
constexpr Support kMatrix[3][static_cast<int>(Feature::Count)] = {
  //                Stats Cache Field Stab
  /* Ayesha  */   {   O,    X,    O,    O },
  /* Escha   */   {   U,    U,    U,    U },
  /* Shallie */   {   U,    U,    U,    U },
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
  if (d.env) {
    if (const char* v = std::getenv(d.env))
      return v[0] != '0';
  }
  return support == Support::OnByDefault;
}

}  // namespace atfix
