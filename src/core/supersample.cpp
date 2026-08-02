// SPDX-License-Identifier: MIT
//
// See supersample.h for why this scales the scene targets rather than
// redirecting the back buffer, and what the first attempt got wrong.
//
// The back-buffer redirect and its box-filter downscale shader are not lost:
// they are in this file's git history, and they remain the right design for a
// renderer that composites into the back buffer -- which is what the Arland
// games do and what Escha & Logy or Shallie may yet turn out to do. They were
// removed rather than left dormant because a second, inert mechanism sitting
// beside the live one is how the wrong one ends up being debugged.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstdlib>

#include "config.h"
#include "game.h"
#include "log.h"
#include "supersample.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The largest scene target this will ask for. Not a driver limit -- feature
// level 11 guarantees 16384 -- but a sanity bound: at 4K a 400% request would
// ask for 15360x8640, half a gigabyte for the colour target alone, before the
// depth target, the MSAA twins, and the engine's own post-processing chain.
constexpr unsigned int kMaxSceneWidth = 7680;
constexpr unsigned int kMaxSceneHeight = 4320;

}  // namespace

unsigned int ssaaPercent() {
  static const unsigned int percent = [] () -> unsigned int {
    if (featureSupport(Feature::Supersampling) == Support::Unsupported)
      return 100;
    int v = 0;
    if (const char* env = std::getenv("DUSK_SSAA"))
      v = std::atoi(env);
    else
      v = duskConfigInt("Rendering", "Supersampling", 100);
    // INTEGER FACTORS ONLY, and this is the substantive difference from the
    // Arland implementation's ladder rather than a restriction for tidiness.
    //
    // There, the mod owns the downscale and applies a box filter sized to the
    // factor, so any factor resamples correctly. Here the ENGINE owns it: its
    // composite pass samples the scene target through an ordinary bilinear
    // sampler, and bilinear is four taps.
    //
    // At exactly 2:1 four taps is the correct answer. A destination pixel
    // centre maps to source coordinate (i + 0.5) * 2 = 2i + 1, which sits
    // exactly between texel centres 2i + 0.5 and 2i + 1.5 -- equal weights on
    // each axis, so the sample is an exact 2x2 box average. Nothing is missed.
    //
    // At 1.5:1 it is not. The footprint is one and a half texels wide while
    // bilinear still reads two, so source texels contribute to no destination
    // pixel at all. The result aliases AND softens: supersampling that looks
    // worse than leaving it off, which is precisely what a 150% run produced.
    //
    // So 125, 150 and 300 are refused rather than honoured badly. If arbitrary
    // factors are wanted here, the work is a mod-owned downscale pass, and the
    // box-filter shader for it is in this file's git history.
    if (v == 200 || v == 400)
      return unsigned(v);
    if (v == 125 || v == 150 || v == 300)
      log("FIXES supersampling=", std::dec, v, "% refused: this engine does its"
          " own downscale with a bilinear sampler, which only resamples"
          " correctly at whole-number factors. Use 200 or 400.");
    return 100;
  }();
  return percent;
}

bool ssaaSceneSize(unsigned int mainWidth, unsigned int mainHeight,
                   unsigned int* sceneWidth, unsigned int* sceneHeight) {
  const unsigned int percent = ssaaPercent();
  if (percent <= 100 || !mainWidth || !mainHeight)
    return false;

  unsigned long long width =
    (static_cast<unsigned long long>(mainWidth) * percent) / 100ull;
  unsigned long long height =
    (static_cast<unsigned long long>(mainHeight) * percent) / 100ull;

  if (width > kMaxSceneWidth || height > kMaxSceneHeight) {
    // Clamp on whichever axis binds first and carry the aspect ratio with it,
    // rather than clamping each independently -- which would stretch the scene.
    const double byWidth = double(kMaxSceneWidth) / double(width);
    const double byHeight = double(kMaxSceneHeight) / double(height);
    const double factor = byWidth < byHeight ? byWidth : byHeight;
    width = static_cast<unsigned long long>(double(width) * factor);
    height = static_cast<unsigned long long>(double(height) * factor);
  }

  // Even dimensions. An odd scene target downscales onto a half-texel offset in
  // the composite, which reads as a picture slightly softer than it should be --
  // exactly the kind of thing that gets blamed on the feature itself.
  *sceneWidth = static_cast<unsigned int>(width) & ~1u;
  *sceneHeight = static_cast<unsigned int>(height) & ~1u;
  return *sceneWidth > mainWidth && *sceneHeight > mainHeight;
}

}  // namespace atfix
