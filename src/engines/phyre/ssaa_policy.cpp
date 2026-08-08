// SPDX-License-Identifier: MIT
#include "ssaa_policy.h"

#include "../../core/highres.h"
#include "../../core/supersample.h"

namespace atfix {
namespace {

// The main render size, which is what this engine composites into. highres.cpp
// is its sole owner.
bool substitutionDestSize(unsigned int* width, unsigned int* height) {
  return highResMainSize(width, height);
}

// ASK THE TARGET THAT IS ACTUALLY BOUND, every time. A size cached when the swap
// chain was created is wrong the moment anything resizes the buffers, and
// reading it once is what put Ayesha's gameplay in the top-left corner of its own
// window: the composite was clamped to the size the back buffer had at startup
// while the buffer itself had since grown.
bool compositeViewportSize(ID3D11DeviceContext* context, unsigned int* width,
                           unsigned int* height) {
  return ssaaBoundColorTargetSize(context, width, height);
}

}  // namespace

const SsaaPolicy& phyreSsaaPolicy() {
  static const SsaaPolicy policy = {
    ssaaConfigured,
    substitutionDestSize,
    compositeViewportSize,
    ssaaNoPolicy().clampPresentSize,   // no swap-chain clamp on this route
    ssaaNoPolicy().fitOutputWindow,
    false,  // no swap-chain clamp, so no DXGI resize hooks
    true,   // the scene test tags the host, and the tag is required
    true,   // without the high-resolution fix nothing is ever enlarged
  };
  return policy;
}

}  // namespace atfix
