// SPDX-License-Identifier: MIT
#pragma once

#include "../../core/supersample_policy.h"

// Ayesha's answers to the questions in supersample_policy.h.
//
// This engine pins several full-screen scene targets at 1920x1080 whatever
// resolution is selected, so supersampling here means enlarging them (highres.cpp
// does that) and owning the resolve: the mod box-filters the enlarged scene down
// and substitutes the result at the composite's sample. There is no swap-chain
// clamp on this route, and the two clamp entries are the inactive policy's
// no-ops rather than anything of Ayesha's own.
namespace atfix {

const SsaaPolicy& phyreSsaaPolicy();

}  // namespace atfix
