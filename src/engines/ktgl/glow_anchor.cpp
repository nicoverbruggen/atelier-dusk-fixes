// SPDX-License-Identifier: MIT
//
// `DUSK_GLOW_TRACE`. See glow_anchor.h for what the anchor is and why the first
// job is to find out whether it fires.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "glow_anchor.h"
#include "../../core/d3d11_hooks.h"
#include "../../core/frame_capture.h"
#include "../../core/frame_map.h"
#include "scene_target.h"
#include "../../core/scene_pass.h"
#include "../../core/smaa.h"
#include "../../core/log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The DXBC container's own checksum, bytes 4..19, as four little-endian dwords,
// with the container's total length from bytes 24..27.
//
// DERIVED, NOT COPIED. Both rows were re-extracted from the shipped
// `Data/x64/PostEffect/pe_pack.elixir.gz` on 2026-08-10: decompress the chunked
// zlib stream, read the 56-byte index records, take `PostEffectGlow.kps`, and
// walk its DXBC containers. #15 is the only one of seventeen that binds two
// textures, and its RDEF names them `smplScene_Tex` and `smplGlowTargetsY3_Tex`.
// Escha's sits at blob offset 0x66cc, Shallie's at 0x5f9c.
//
// THE BYTE ORDER MATTERS. Written here as the dwords a little-endian read
// produces, which is the reverse of the raw byte sequence on disk. A memcmp
// against the container using the on-disk order fails silently and looks exactly
// like an anchor that never fires.
struct GlowShader {
  const char* executable;
  uint32_t checksum[4];
  uint32_t length;
};

constexpr GlowShader kGlowShaders[] = {
  { "Atelier_Escha_and_Logy_EN.exe",
    { 0x5e6c7dac, 0xe8a0ff15, 0xb4615d66, 0x6a3f46b3 }, 1144 },
  { "Atelier_Shallie_EN.exe",
    { 0x0e6dfd16, 0x3b68523a, 0x3267eeb3, 0x4fdc1df1 }, 1112 },
  // The multilingual builds ship their own packs and have not been extracted.
  // A missing row declines rather than guesses.
};

const GlowShader* g_row = nullptr;
std::atomic<bool> g_tracing{false};

// The composite, once the device has created it. Compared by pointer at the
// bind, which is exact and costs nothing on a call this hot.
std::atomic<void*> g_composite{nullptr};

std::atomic<uint64_t> g_shadersSeen{0}, g_binds{0}, g_framesWithBind{0};

// WHAT RUNS AFTER THE COMPOSITE, which is the last thing that decides whether
// this anchor can carry a pre-UI pass. `PostEffectGlow` is one of sixteen passes
// in Escha and seventeen in Shallie; `Color`, `Gamma`, `Wave`, `Fade` and
// Shallie's `DOF` are separate files that may run after it. Antialiasing before
// a Wave distortion would be smeared.
//
// The checksums are logged rather than matched against a built-in table, and
// then resolved offline against the shipped pack. A table of two hundred
// shaders would have to be right to be useful; a checksum in the log is right by
// construction and costs nothing to re-derive.
struct SeenShader {
  void* shader;
  uint32_t checksum[4];
  uint32_t length;
};
constexpr int kMaxShaders = 512;
SeenShader g_seen[kMaxShaders];
std::atomic<int> g_seenCount{0};

// Set when the composite is bound, cleared at Present. Only shaders bound while
// it is set are of interest.
std::atomic<bool> g_afterComposite{false};

// HOW FAR THROUGH THE FRAME THE COMPOSITE SITS, which decides whether this
// anchor is pre-UI at all. Only one distinct shader was ever seen after it,
// and the interface has to be drawn with something -- so either the interface
// precedes it, or the composite is near the end of the frame and injecting
// there would antialias the interface too, which is what the Present-time pass
// already does and why it is opt-in.
//
// Counted rather than reasoned about: binds before and after, per frame.
std::atomic<uint32_t> g_bindsThisFrame{0};
std::atomic<uint32_t> g_bindsBefore{0};
std::atomic<uint64_t> g_sumBefore{0}, g_sumAfter{0}, g_framesCounted{0};

// BINDS ARE NOT DRAWS, and the difference decides this feature. A 2D interface
// can bind one pixel shader and issue hundreds of draws with it, so "2 binds
// after the composite" is equally consistent with "nothing follows" and with
// "the entire interface follows". Counting draws separates them.
std::atomic<uint32_t> g_drawsThisFrame{0};
std::atomic<uint32_t> g_drawsBefore{0};
std::atomic<uint64_t> g_sumDrawsBefore{0}, g_sumDrawsAfter{0};

// TWO PICTURES, ONCE, and they answer what the counts could not. Between them:
// 276 draws precede the composite and 12 to 18 follow it, which is consistent
// both with "the interface is already composited" and with "a small heads-up
// display follows". Dumping the composite's own render target immediately after
// its draw settles it by looking -- interface in the image means this anchor is
// post-UI and cannot carry a pre-UI pass.
//
// Delayed by a few hundred compositing frames so the shot lands in settled
// gameplay rather than the first frame of a fade-in.
constexpr uint64_t kDumpAfterComposites = 900;
std::atomic<bool> g_dumped{false};
std::atomic<ID3D11Texture2D*> g_sourceToDump{nullptr};

// `DUSK_SMAA_AT_GLOW=1` antialiases the surface the composite is about to read,
// at the composite's own draw.
//
// TESTED DIRECTLY RATHER THAN DIAGNOSED FURTHER. Every proxy measurement tonight
// answered a slightly different question than the one asked -- leave counts,
// accepted-surface counts, two texture dumps taken at record time on a deferred
// context. This applies the pass where the argument says it belongs and shows
// the result on screen, which is the only test that cannot be misread: if the
// world smooths and the interface stays sharp it is right, and if the interface
// softens it is post-UI and the route is dead.
bool smaaAtGlow() {
  static const bool on = [] {
    const char* env = std::getenv("DUSK_SMAA_AT_GLOW");
    return env && env[0] != '0';
  }();
  return on;
}
std::atomic<bool> g_armDump{false};
constexpr int kAfterLogLimit = 24;
std::atomic<int> g_afterLogged{0};

const SeenShader* lookup(void* shader) {
  const int n = g_seenCount.load(std::memory_order_relaxed);
  for (int i = 0; i < n && i < kMaxShaders; ++i)
    if (g_seen[i].shader == shader)
      return &g_seen[i];
  return nullptr;
}

bool matchesGlow(const void* bytecode, SIZE_T length) {
  if (!g_row || !bytecode || length < 32)
    return false;
  if (uint32_t(length) != g_row->length)
    return false;
  const uint8_t* p = static_cast<const uint8_t*>(bytecode);
  if (std::memcmp(p, "DXBC", 4) != 0)
    return false;
  uint32_t checksum[4] = {};
  std::memcpy(checksum, p + 4, sizeof(checksum));
  return std::memcmp(checksum, g_row->checksum, sizeof(checksum)) == 0;
}

}  // namespace

// The draw counters the trace keeps, fed from the pre-UI feature's detours so
// only one set of draw hooks exists on the vtable.
bool glowTraceEnabled() {
  static const bool on = [] {
    const char* env = std::getenv("DUSK_GLOW_TRACE");
    return env && env[0] != '0';
  }();
  return on;
}

HRESULT STDMETHODCALLTYPE hookedCreatePixelShader(
    ID3D11Device* self, const void* bytecode, SIZE_T length,
    ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
  const HRESULT hr = d3d11DeviceOriginals().createPixelShader(
    self, bytecode, length, linkage, out);
  if (FAILED(hr) || !out || !*out)
    return hr;

  g_shadersSeen.fetch_add(1, std::memory_order_relaxed);
  const int slot = g_seenCount.fetch_add(1, std::memory_order_relaxed);
  if (slot < kMaxShaders && length >= 20) {
    g_seen[slot].shader = *out;
    std::memcpy(g_seen[slot].checksum, static_cast<const uint8_t*>(bytecode) + 4,
                sizeof(g_seen[slot].checksum));
    g_seen[slot].length = uint32_t(length);
  }
  if (matchesGlow(bytecode, length)) {
    g_composite.store(*out, std::memory_order_relaxed);
    log("GLOW: composite identified -- ", std::dec, unsigned(length),
        " bytes, checksum matches the pack's PostEffectGlow.kps #15"
        " (the one binding smplScene_Tex and smplGlowTargetsY3_Tex)");
  }
  return hr;
}

void STDMETHODCALLTYPE hookedPSSetShader(
    ID3D11DeviceContext* self, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* instances, UINT numInstances) {
  if (g_tracing.load(std::memory_order_relaxed) && shader &&
      shader == g_composite.load(std::memory_order_relaxed)) {
    // Counted rather than logged per bind: this fires once or twice a frame and
    // a line each would be thousands. The count is what answers the question the
    // third-party mod got wrong, which is whether it fires at all.
    const uint64_t n = g_binds.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1)
      log("GLOW: composite BOUND for the first time -- the anchor is live");
    g_afterComposite.store(true, std::memory_order_relaxed);
    g_bindsBefore.store(g_bindsThisFrame.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    g_drawsBefore.store(g_drawsThisFrame.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    if (!g_dumped.load(std::memory_order_relaxed) && n >= kDumpAfterComposites)
      g_armDump.store(true, std::memory_order_relaxed);
  } else if (g_tracing.load(std::memory_order_relaxed) && shader &&
             g_afterComposite.load(std::memory_order_relaxed) &&
             g_afterLogged.load(std::memory_order_relaxed) < kAfterLogLimit) {
    if (const SeenShader* seen = lookup(shader)) {
      // Distinct shaders only, so one busy frame does not fill the budget with
      // the interface's repeats.
      static uint32_t logged[kAfterLogLimit][4] = {};
      const int have = g_afterLogged.load(std::memory_order_relaxed);
      bool known = false;
      for (int i = 0; i < have && !known; ++i)
        known = std::memcmp(logged[i], seen->checksum, sizeof(logged[i])) == 0;
      if (!known) {
        const int at = g_afterLogged.fetch_add(1, std::memory_order_relaxed);
        if (at < kAfterLogLimit) {
          std::memcpy(logged[at], seen->checksum, sizeof(logged[at]));
          log("GLOW after: len=", std::dec, seen->length, " checksum=",
              std::hex, seen->checksum[0], " ", seen->checksum[1], " ",
              seen->checksum[2], " ", seen->checksum[3], std::dec);
        }
      }
    }
  }
  if (g_tracing.load(std::memory_order_relaxed))
    g_bindsThisFrame.fetch_add(1, std::memory_order_relaxed);
  d3d11OriginalsFor(self).psSetShader(self, shader, instances, numInstances);
}

void glowTraceFrameTick() {
  if (!g_tracing.load(std::memory_order_relaxed))
    return;

  // At Present the frame's command lists have run, so this surface holds what
  // the composite will actually read rather than what preceded it.
  if (ID3D11Texture2D* source =
        g_sourceToDump.exchange(nullptr, std::memory_order_relaxed)) {
    log("GLOW: dumping the composite's SOURCE at Present -- no interface in it"
        " means antialiasing there would be pre-UI and would reach the screen");
    frameCaptureDumpTexture(source, "dusk-glow-source");
    source->Release();
  }
  static std::atomic<uint64_t> frames{0};
  static std::atomic<uint64_t> lastBinds{0};
  const uint64_t frame = frames.fetch_add(1, std::memory_order_relaxed) + 1;
  // Only frames that actually ran the composite say anything about its position.
  if (g_afterComposite.load(std::memory_order_relaxed)) {
    const uint32_t total = g_bindsThisFrame.load(std::memory_order_relaxed);
    const uint32_t before = g_bindsBefore.load(std::memory_order_relaxed);
    g_sumBefore.fetch_add(before, std::memory_order_relaxed);
    g_sumAfter.fetch_add(total > before ? total - before : 0,
                         std::memory_order_relaxed);
    const uint32_t draws = g_drawsThisFrame.load(std::memory_order_relaxed);
    const uint32_t drawsBefore = g_drawsBefore.load(std::memory_order_relaxed);
    g_sumDrawsBefore.fetch_add(drawsBefore, std::memory_order_relaxed);
    g_sumDrawsAfter.fetch_add(draws > drawsBefore ? draws - drawsBefore : 0,
                              std::memory_order_relaxed);
    g_framesCounted.fetch_add(1, std::memory_order_relaxed);
  }
  g_afterComposite.store(false, std::memory_order_relaxed);
  g_bindsThisFrame.store(0, std::memory_order_relaxed);
  g_bindsBefore.store(0, std::memory_order_relaxed);
  g_drawsThisFrame.store(0, std::memory_order_relaxed);
  g_drawsBefore.store(0, std::memory_order_relaxed);
  const uint64_t binds = g_binds.load(std::memory_order_relaxed);
  if (binds != lastBinds.exchange(binds, std::memory_order_relaxed))
    g_framesWithBind.fetch_add(1, std::memory_order_relaxed);

  if (frame % 600 != 0)
    return;
  const uint64_t withBind = g_framesWithBind.load(std::memory_order_relaxed);
  log("GLOW frames=", std::dec, frame,
      " shadersCreated=", g_shadersSeen.load(std::memory_order_relaxed),
      " compositeBinds=", binds,
      " framesWithABind=", withBind,
      " (", frame ? (withBind * 100 / frame) : 0, "% of frames)");
  const uint64_t counted = g_framesCounted.load(std::memory_order_relaxed);
  if (counted)
    log("GLOW position: of the shader binds in a compositing frame, ",
        std::dec, g_sumBefore.load(std::memory_order_relaxed) / counted,
        " come before the composite and ",
        g_sumAfter.load(std::memory_order_relaxed) / counted, " after"
        " (averaged over ", counted, " frames)");
  if (counted)
    log("GLOW draws: ", std::dec,
        g_sumDrawsBefore.load(std::memory_order_relaxed) / counted,
        " before the composite, ",
        g_sumDrawsAfter.load(std::memory_order_relaxed) / counted, " after"
        " -- a large number after means the interface follows it, which would"
        " make this anchor post-UI and useless for the purpose");
}

bool installGlowTrace(const KtglGame& game) {
  if (!glowTraceEnabled())
    return false;
  for (const GlowShader& row : kGlowShaders) {
    if (std::strcmp(row.executable, game.executable) == 0) {
      g_row = &row;
      break;
    }
  }
  if (!g_row) {
    log("GLOW trace: no shader row for this build; the pack has not been"
        " extracted for it");
    return false;
  }
  g_tracing.store(true, std::memory_order_relaxed);
  log("GLOW trace: active, watching for a ", std::dec, g_row->length,
      "-byte pixel shader (nothing is changed)");
  return true;
}

// WHAT THE COMPOSITE ACTUALLY SAMPLES AS THE SCENE, against what the scene test
// accepted. The pre-UI pass fires on the accepted surface once per frame at the
// only transition there is, and produces nothing visible. If the composite reads
// a different texture, that is the whole explanation.
void compareCompositeSource(ID3D11DeviceContext* self) {
  static std::atomic<bool> checked{false};
  if (checked.load(std::memory_order_relaxed))
    return;
  // ONLY THE COMPOSITE'S OWN DRAW. Without this the comparison samples whatever
  // the first draw of the frame had bound, which is not the question.
  if (!g_afterComposite.load(std::memory_order_relaxed))
    return;
  void* accepted = scenePassAcceptedSurface();
  if (!accepted)
    return;   // nothing to compare against yet
  ID3D11ShaderResourceView* srv = nullptr;
  self->PSGetShaderResources(0, 1, &srv);
  if (!srv)
    return;
  ID3D11Resource* resource = nullptr;
  srv->GetResource(&resource);
  srv->Release();
  if (!resource)
    return;
  ID3D11Texture2D* texture = nullptr;
  resource->QueryInterface(IID_ID3D11Texture2D,
                           reinterpret_cast<void**>(&texture));
  resource->Release();
  if (!texture)
    return;
  if (smaaAtGlow()) {
    // On the recording context, like any other pass the frame issues, so it is
    // ordered correctly against the composite that follows it.
    static std::atomic<bool> said{false};
    if (!said.exchange(true, std::memory_order_relaxed))
      log("GLOW: applying SMAA to the composite's source before it draws");
    smaaApplySceneColor(self, texture);
  }
  if (!checked.exchange(true, std::memory_order_relaxed)) {
    D3D11_TEXTURE2D_DESC td = {};
    texture->GetDesc(&td);
    // THE SOURCE, NOT THE TARGET, and the distinction was muddled once already
    // tonight. The earlier dump that showed the interface was the composite's
    // RENDER TARGET -- what it draws into, which the interface was already in.
    // This is what it READS as the scene. If that is scene-only, antialiasing it
    // immediately before this draw is pre-UI by construction, and it is the
    // surface the composite actually consumes rather than the one the scene test
    // accepted and nothing reads.
    // REMEMBERED, NOT DUMPED HERE. This engine records on deferred contexts,
    // so at the moment this draw is RECORDED the texture holds whatever was in
    // it before the command list ran -- which came back uniformly black. The
    // dump has to happen once the list has executed, which is what Present
    // guarantees, so the surface is held and written there instead.
    //
    // The earlier dump of the composite's render TARGET was taken this same
    // wrong way and cannot be trusted either. It appeared to show the interface
    // and was read as proof the anchor is post-UI; what it actually showed may
    // have been the previous frame's finished image.
    texture->AddRef();
    g_sourceToDump.store(texture, std::memory_order_relaxed);
    log("GLOW: holding the composite's SOURCE for a dump at Present, after the"
        " deferred list has executed");
    const bool same = static_cast<void*>(texture) == accepted;
    log("GLOW source: the composite samples ", std::dec, td.Width, "x",
        td.Height, " format=", unsigned(td.Format),
        " bind=0x", std::hex, td.BindFlags, std::dec,
        same ? " -- SAME surface the scene test accepted"
             : " -- a DIFFERENT surface from the one the scene test accepted,"
               " so the pre-UI pass is antialiasing something the composite"
               " never reads");
  }
  texture->Release();
}

// After the composite's own draw, so the image includes what it just wrote.
void dumpCompositeTarget(ID3D11DeviceContext* self) {
  if (!g_armDump.exchange(false, std::memory_order_relaxed))
    return;
  if (g_dumped.exchange(true, std::memory_order_relaxed))
    return;
  ID3D11RenderTargetView* rtv = nullptr;
  self->OMGetRenderTargets(1, &rtv, nullptr);
  if (!rtv) {
    log("GLOW: the composite draw had no colour target bound");
    return;
  }
  ID3D11Resource* resource = nullptr;
  rtv->GetResource(&resource);
  rtv->Release();
  if (!resource)
    return;
  ID3D11Texture2D* texture = nullptr;
  resource->QueryInterface(IID_ID3D11Texture2D,
                           reinterpret_cast<void**>(&texture));
  resource->Release();
  if (!texture)
    return;
  log("GLOW: dumping the composite's render target -- interface visible in it"
      " means this anchor is post-UI");
  frameCaptureDumpTexture(texture, "dusk-glow-composite");
  texture->Release();
}





// The draw counters the trace keeps, fed from the pre-UI feature's draw
// detours so only one set of draw hooks exists on the vtable.
void glowTraceNoteDraw(ID3D11DeviceContext* self) {
  if (g_tracing.load(std::memory_order_relaxed))
    g_drawsThisFrame.fetch_add(1, std::memory_order_relaxed);
  compareCompositeSource(self);
  dumpCompositeTarget(self);
}

}  // namespace atfix
