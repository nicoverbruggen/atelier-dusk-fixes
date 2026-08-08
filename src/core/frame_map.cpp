// SPDX-License-Identifier: MIT
//
// `DUSK_FRAME_MAP`. See frame_map.h for why this exists instead of a ninth
// narrow probe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "frame_map.h"
#include "frame_capture.h"
#include "d3d11_hooks.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

std::atomic<uint64_t> g_frame{0};

uint64_t targetFrame() {
  static const uint64_t n = [] () -> uint64_t {
    const char* env = std::getenv("DUSK_FRAME_MAP");
    if (!env || !env[0])
      return 0;
    const long long v = std::atoll(env);
    return v > 0 ? uint64_t(v) : 0;
  }();
  return n;
}

bool recording() {
  const uint64_t want = targetFrame();
  return want && g_frame.load(std::memory_order_relaxed) == want;
}

// Distinct colour surfaces this frame, in the order first seen. The index is
// what the event list refers to, so the sequence stays readable.
constexpr int kMaxSurfaces = 12;
struct Surface {
  ID3D11Texture2D* texture;   // referenced while held
  UINT width, height, format, bind;
  bool fullScreen;
};
Surface g_surfaces[kMaxSurfaces];
std::atomic<int> g_surfaceCount{0};

// The event list. Small and fixed: one frame of a title-screen-sized workload is
// a few hundred entries and the interesting structure is in the first dozen.
constexpr int kMaxEvents = 512;
struct Event {
  char kind;      // 'T' target bind, 'S' shader bind, 'C' composite bind, 'D' draws
  int surface;    // index into g_surfaces, or -1
  uint32_t count; // draws, for 'D'
};
Event g_events[kMaxEvents];
std::atomic<int> g_eventCount{0};
std::atomic<uint32_t> g_drawsSinceEvent{0};

// `DUSK_FRAME_MAP_AT=<surface>:<draw>` copies a surface as it stands after the
// Nth draw following its bind, and writes that copy at Present.
//
// THIS IS THE ONLY HONEST WAY TO SEE A SURFACE MID-FRAME on this engine. It
// records on deferred contexts, so a copy issued from the immediate context at
// record time reads whatever preceded the frame -- two dumps today came back
// black or stale for exactly that reason. The copy has to be RECORDED into the
// same command stream, so it executes in order with the draws around it, and
// only then read.
int g_watchSurface = -1, g_watchDraw = -1;
void parseWatch() {
  static bool done = false;
  if (done) return;
  done = true;
  const char* env = std::getenv("DUSK_FRAME_MAP_AT");
  if (!env || !env[0]) return;
  std::sscanf(env, "%d:%d", &g_watchSurface, &g_watchDraw);
}
ID3D11Texture2D* g_watchCopy = nullptr;
int g_watchBoundAt = -1;        // event index when the surface was last bound
int g_drawsSinceWatchBind = -1; // -1 = not armed
bool g_watchTaken = false;

void pushEvent(char kind, int surface, uint32_t count) {
  const int at = g_eventCount.fetch_add(1, std::memory_order_relaxed);
  if (at >= kMaxEvents)
    return;
  g_events[at].kind = kind;
  g_events[at].surface = surface;
  g_events[at].count = count;
}

// Draws are collapsed into runs, because a frame issues hundreds and what
// matters is how many happened between one bind and the next.
void flushDraws() {
  const uint32_t n = g_drawsSinceEvent.exchange(0, std::memory_order_relaxed);
  if (n)
    pushEvent('D', -1, n);
}

int noteSurface(ID3D11Texture2D* texture) {
  const int n = g_surfaceCount.load(std::memory_order_relaxed);
  for (int i = 0; i < n && i < kMaxSurfaces; ++i)
    if (g_surfaces[i].texture == texture)
      return i;
  if (n >= kMaxSurfaces)
    return -1;
  D3D11_TEXTURE2D_DESC d = {};
  texture->GetDesc(&d);
  texture->AddRef();   // held until the dump at Present
  g_surfaces[n].texture = texture;
  g_surfaces[n].width = d.Width;
  g_surfaces[n].height = d.Height;
  g_surfaces[n].format = d.Format;
  g_surfaces[n].bind = d.BindFlags;
  // Only full-screen colour surfaces are worth an image; the blur pyramid and
  // the shadow map are not what a pre-UI pass would attach to.
  g_surfaces[n].fullScreen = d.Width >= 1280 && d.Height >= 720 &&
                             (d.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
  g_surfaceCount.store(n + 1, std::memory_order_relaxed);
  return n;
}

}  // namespace

bool frameMapEnabled() {
  return targetFrame() != 0;
}

void frameMapNoteTargets(ID3D11DeviceContext*, unsigned int numViews,
                         ID3D11RenderTargetView* const* views) {
  if (!recording() || !numViews || !views || !views[0])
    return;
  ID3D11Resource* resource = nullptr;
  views[0]->GetResource(&resource);
  if (!resource)
    return;
  ID3D11Texture2D* texture = nullptr;
  resource->QueryInterface(IID_ID3D11Texture2D,
                           reinterpret_cast<void**>(&texture));
  resource->Release();
  if (!texture)
    return;
  flushDraws();
  const int index = noteSurface(texture);
  pushEvent('T', index, 0);
  parseWatch();
  if (index == g_watchSurface && !g_watchTaken)
    g_drawsSinceWatchBind = 0;
  texture->Release();   // noteSurface took its own reference
}

void frameMapNoteShader(ID3D11DeviceContext*, void*, bool isComposite) {
  if (!recording() || !isComposite)
    return;
  flushDraws();
  pushEvent('C', -1, 0);
}

void frameMapNoteDraw(ID3D11DeviceContext* context) {
  if (!recording())
    return;
  g_drawsSinceEvent.fetch_add(1, std::memory_order_relaxed);

  parseWatch();
  if (g_watchSurface < 0 || g_watchTaken || g_drawsSinceWatchBind < 0)
    return;
  ++g_drawsSinceWatchBind;
  if (g_drawsSinceWatchBind != g_watchDraw)
    return;
  g_watchTaken = true;
  if (g_watchSurface >= g_surfaceCount.load(std::memory_order_relaxed))
    return;
  ID3D11Texture2D* src = g_surfaces[g_watchSurface].texture;
  if (!src)
    return;
  ID3D11Device* device = nullptr;
  src->GetDevice(&device);
  if (!device)
    return;
  D3D11_TEXTURE2D_DESC d = {};
  src->GetDesc(&d);
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.CPUAccessFlags = 0;
  d.MiscFlags = 0;
  if (SUCCEEDED(createTexture2DUnhooked(device, &d, nullptr, &g_watchCopy)) &&
      g_watchCopy)
    context->CopyResource(g_watchCopy, src);   // RECORDED, in stream order
  device->Release();
}

void frameMapFrameTick() {
  if (!targetFrame())
    return;
  const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frame != targetFrame() + 1)
    return;   // the recorded frame has just ended

  flushDraws();
  const int events = g_eventCount.load(std::memory_order_relaxed);
  const int surfaces = g_surfaceCount.load(std::memory_order_relaxed);
  log("FRAMEMAP frame ", std::dec, targetFrame(), ": ", surfaces,
      " distinct colour surfaces, ", events > kMaxEvents ? kMaxEvents : events,
      " events");

  for (int i = 0; i < surfaces && i < kMaxSurfaces; ++i) {
    const Surface& s = g_surfaces[i];
    log("  surface #", std::dec, i, "  ", s.width, "x", s.height,
        " format=", s.format, " bind=0x", std::hex, s.bind, std::dec,
        s.fullScreen ? "  (dumped)" : "");
  }

  // The sequence, on one line per run of events, so the shape of the frame is
  // readable rather than reconstructed from hundreds of lines.
  std::string line;
  char buf[32] = {};
  for (int i = 0; i < events && i < kMaxEvents; ++i) {
    const Event& e = g_events[i];
    if (e.kind == 'T')
      std::snprintf(buf, sizeof(buf), " T%d", e.surface);
    else if (e.kind == 'C')
      std::snprintf(buf, sizeof(buf), " [COMPOSITE]");
    else
      std::snprintf(buf, sizeof(buf), " %ud", e.count);
    line += buf;
    if (line.size() > 160) {
      log("  seq", line.c_str());
      line.clear();
    }
  }
  if (!line.empty())
    log("  seq", line.c_str());

  // Now the images, from the immediate context with everything executed.
  for (int i = 0; i < surfaces && i < kMaxSurfaces; ++i) {
    if (!g_surfaces[i].fullScreen)
      continue;
    char name[64] = {};
    std::snprintf(name, sizeof(name), "dusk-map-%d", i);
    frameCaptureDumpTexture(g_surfaces[i].texture, name);
  }
  if (g_watchCopy) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "dusk-map-%d-after%d", g_watchSurface,
                  g_watchDraw);
    log("FRAMEMAP: writing surface #", std::dec, g_watchSurface, " as it stood"
        " after draw ", g_watchDraw, " of its bind");
    frameCaptureDumpTexture(g_watchCopy, name);
    g_watchCopy->Release();
    g_watchCopy = nullptr;
  }
  log("FRAMEMAP: surfaces written; the one holding the finished scene WITHOUT"
      " the interface is where a pre-UI pass belongs");

  for (int i = 0; i < surfaces && i < kMaxSurfaces; ++i)
    if (g_surfaces[i].texture) {
      g_surfaces[i].texture->Release();
      g_surfaces[i].texture = nullptr;
    }
}

}  // namespace atfix
