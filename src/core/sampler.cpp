// SPDX-License-Identifier: MIT
//
// See sampler.h for the rule and for the open question about point samplers.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "config.h"
#include "d3d11_hooks.h"
#include "game.h"
#include "log.h"
#include "sampler.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

std::atomic<uint64_t> g_upgradedFromLinear{0};
std::atomic<uint64_t> g_upgradedFromPoint{0};
std::atomic<uint64_t> g_leftAlone{0};

// Whether point samplers keep their filter. Off by default, matching Arland --
// but reachable, because the reason to doubt the blanket upgrade is specific
// and this is cheaper than reasoning about it. See sampler.h.
bool keepPointSamplers() {
  static const bool keep = [] {
    const char* env = std::getenv("DUSK_ANISO_KEEP_POINT");
    return env && env[0] != '0';
  }();
  return keep;
}

}  // namespace

unsigned int anisotropyLevel() {
  static const unsigned int level = [] () -> unsigned int {
    const Support support = featureSupport(Feature::AnisotropicFiltering);
    if (support == Support::Unsupported)
      return 0;
    // Ayesha uses 16x rather than Arland's original 8x and ships it on: that
    // path is runtime-validated and reported no point samplers. The KTGL rows
    // are Unsupported, so the early return above makes their ini key and
    // environment override inert before either is read.
    const int fallback = support == Support::OnByDefault ? 16 : 0;
    int v = 0;
    if (const char* env = std::getenv("DUSK_ANISO"))
      v = std::atoi(env);
    else
      v = duskConfigInt("Rendering", "AnisotropicFiltering", fallback);
    if (v == 2 || v == 4 || v == 8 || v == 16)
      return unsigned(v);
    return 0;
  }();
  return level;
}

bool samplerUpgrade(D3D11_SAMPLER_DESC* desc) {
  const unsigned int aniso = anisotropyLevel();
  if (!aniso || !desc)
    return false;

  // Above the basic point/linear block this is either already anisotropic or a
  // comparison/min/max filter doing something else entirely.
  if (desc->Filter > D3D11_FILTER_MIN_MAG_MIP_LINEAR) {
    g_leftAlone.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const bool isPoint = desc->Filter == D3D11_FILTER_MIN_MAG_MIP_POINT;
  if (isPoint && keepPointSamplers()) {
    g_leftAlone.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  desc->Filter = D3D11_FILTER_ANISOTROPIC;
  desc->MaxAnisotropy = aniso;
  if (isPoint)
    g_upgradedFromPoint.fetch_add(1, std::memory_order_relaxed);
  else
    g_upgradedFromLinear.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void samplerReport() {
  if (!anisotropyLevel())
    return;
  // Periodic, not once.
  //
  // This latched on a single snapshot and lied about it. Sampler states are
  // created as content loads, not all at startup, so an early reading is a
  // partial one -- a run reported `fromPoint=0` and the conclusion drawn was
  // "this game has no point samplers, the question is closed". The next run
  // reported `fromPoint=1`. The count only means something if it is allowed to
  // grow, so it is reported whenever it changes.
  static std::atomic<uint64_t> frames{0};
  if ((frames.fetch_add(1, std::memory_order_relaxed) % 600) != 599)
    return;
  const uint64_t fromLinear = g_upgradedFromLinear.load(std::memory_order_relaxed);
  const uint64_t fromPoint = g_upgradedFromPoint.load(std::memory_order_relaxed);
  const uint64_t total = fromLinear + fromPoint;
  static std::atomic<uint64_t> lastTotal{0};
  if (total == lastTotal.load(std::memory_order_relaxed))
    return;
  lastTotal.store(total, std::memory_order_relaxed);
  // fromPoint is the number that matters. Point sampling is *correct* for
  // lookup textures and for anything meant to stay crisp, so a non-zero count
  // is a reason to look at the picture, not a milestone.
  log("FIXES anisotropic=", std::dec, anisotropyLevel(),
      "x upgraded fromLinear=", fromLinear,
      " fromPoint=", fromPoint,
      " leftAlone=", g_leftAlone.load(std::memory_order_relaxed),
      keepPointSamplers() ? " (point samplers kept)" : "");
}

HRESULT STDMETHODCALLTYPE hookedCreateSamplerState(
    ID3D11Device* self, const D3D11_SAMPLER_DESC* desc,
    ID3D11SamplerState** state) {
  if (!desc)
    return d3d11DeviceOriginals().createSamplerState(self, desc, state);
  D3D11_SAMPLER_DESC local = *desc;
  if (samplerUpgrade(&local))
    return d3d11DeviceOriginals().createSamplerState(self, &local, state);
  return d3d11DeviceOriginals().createSamplerState(self, desc, state);
}

}  // namespace atfix
