// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// The 16:9 render size to write into a game's own Setting.ini, shared by the
// two places that write it: the launcher window when it saves, and the proxy
// when it re-resolves Auto on a redirected start. Both used to write the
// desktop's own width and height, which is what made the picture stretch on a
// display that is not 16:9.
//
// WHO GETS IT. PhyreEngine always, because on that engine a 16:9 render size is
// the whole correction -- core/letterbox.h has why. KTGL only in a window:
// fullscreen keeps a 4:3 base so its Present-time pass fits the frame inside the
// panel (engines/ktgl/letterbox_present.h), and fitting the base as well would
// hand that pass an already-fitted frame to fit again.
//
// Even dimensions, because an odd render size lands a downscale on a half-texel
// offset. A size already 16:9 comes back untouched.
namespace atfix {

inline void fitRenderToSixteenNine(unsigned* width, unsigned* height) {
  if (!width || !height || !*width || !*height)
    return;
  const std::uint64_t wide = std::uint64_t(*width) * 9;
  const std::uint64_t tall = std::uint64_t(*height) * 16;
  if (wide == tall)
    return;

  unsigned fittedWidth = *width;
  unsigned fittedHeight = *height;
  if (wide > tall)
    fittedWidth = unsigned(std::uint64_t(*height) * 16 / 9);
  else
    fittedHeight = unsigned(std::uint64_t(*width) * 9 / 16);
  fittedWidth &= ~1u;
  fittedHeight &= ~1u;

  // A panel that is ALMOST 16:9 is left alone, the same one-percent tolerance
  // the rest of this correction uses. 1366x768 is off by 0.05%, and correcting
  // it would buy a two-pixel bar down each side.
  const unsigned lostWidth = *width - fittedWidth;
  const unsigned lostHeight = *height - fittedHeight;
  if (std::uint64_t(lostWidth) * 100 < *width &&
      std::uint64_t(lostHeight) * 100 < *height)
    return;

  *width = fittedWidth;
  *height = fittedHeight;
}

}  // namespace atfix
