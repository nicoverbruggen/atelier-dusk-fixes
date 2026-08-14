// SPDX-License-Identifier: MIT
//
// See supersample.h for the mechanism, the four attempts that preceded it, and
// why the composite is identified by the back buffer rather than inferred from
// a scene-target transition.
//
// TWO RULES HOLD THIS FILE TOGETHER, and both were bought with a failed run:
//
//   Every internal bind goes through a TRAMPOLINE, never through the public
//   ID3D11DeviceContext method. The fourth attempt called
//   context->OMSetRenderTargets inside its pass, re-entered the scene-pass
//   detour that hook lives in, and hung the game on the loading screen before the
//   intro video could play. Draw, RSSetViewports, RSSetScissorRects,
//   PSSetShaderResources and OMSetRenderTargets are all hooked in this project;
//   every one of them is reached here through d3d11OriginalsFor().
//
//   Nothing this module computes is latched past the call that needs it. The
//   substitution exists only in the argument array of the single
//   PSSetShaderResources call it was computed for. A substitution that stayed
//   armed would be handed to the post-processing passes that sample the same
//   texture, which is the failure that made attempt 4's picture black.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "game.h"
#include "log.h"
#include "d3d11_hooks.h"
#include "highres.h"
#include "sharpen.h"
#include "smaa.h"
#include "supersample.h"
#include "supersample_policy.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The largest scene target this will ask for. Not a driver limit -- feature
// level 11 guarantees 16384 -- but a sanity bound: at 4K a 400% request would
// ask for 15360x8640, half a gigabyte for the colour target alone, before the
// depth target and the engine's own post-processing chain.
constexpr unsigned int kMaxSceneWidth = 7680;
constexpr unsigned int kMaxSceneHeight = 4320;

}  // namespace

unsigned int ssaaPercent() {
  static const unsigned int percent = [] () -> unsigned int {
    if (featureSupport(Feature::Supersampling) == Support::Unsupported)
      return 100;
    int v = 0;
    // Read directly rather than through featureEnabled(): this setting is an
    // INT percentage, and the capability matrix's boolean path would seed
    // `false` into the ini key the moment anything asked whether the feature
    // was on. The matrix still owns whether the running game supports it at
    // all, which is the check above.
    if (const char* env = std::getenv("DUSK_SSAA"))
      v = std::atoi(env);
    else
      v = duskConfigInt("Rendering", "Supersampling", 100);
    // The full ladder, matching Arland's.
    //
    // This was briefly restricted to whole-number factors, and that restriction
    // was correct for a version of this feature that no longer exists: when the
    // ENGINE owned the downscale, its composite sampled through a bilinear
    // sampler, four taps resample correctly only at a whole-number ratio, and
    // 150% aliased and softened at the same time. The downscale is now this
    // module's own, box-filtered with ceil(ratio) samples per axis, which is
    // the same shader Arland runs at 125% and 150%. The restriction outlived
    // its reason by several builds and silently refused the setting the ini
    // actually asked for.
    //
    // Fractional factors use this path in all three games. Ayesha's scene route
    // and both KTGL whole-frame routes have been validated in game, including
    // 150% on Escha & Logy and Shallie.
    if (v == 125 || v == 150 || v == 200 || v == 300 || v == 400)
      return unsigned(v);
    return 100;
  }();
  return percent;
}

bool ssaaConfigured() {
  return ssaaPercent() > 100;
}

// Fixed, and not a setting. This compensates for a blur THIS pass introduces:
// the box filter below averages, an average is softer than its source, and the
// four extra taps that undo it are already in cache. That makes it a correction
// rather than a preference, and nobody has a considered opinion about the right
// percentage to cancel a box filter. `[Rendering] Sharpen` is the sharpening
// control a player has, and it runs afterwards as its own CAS pass.
//
// It was configurable through `[Rendering] SupersamplingSharpen` and
// DUSK_SSAA_SHARPEN. Both were removed once the separate sharpening pass
// shipped, because two sharpening numbers invited tuning one against the other.
constexpr float kDownscaleSharpen = 0.35f;

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


// ---- identity tags ---------------------------------------------------------

namespace {

// Private-data keys. A distinct base from scene_pass.cpp's, and from the Arland
// project's: the two mods are never loaded into the same process, but colliding
// GUIDs across sibling codebases costs a day to find if it ever happens.
//
// On a RESOURCE: this is the swap chain's back buffer.
const GUID IID_DuskBackBuffer =
  { 0x9d3e7b11, 0x2c48, 0x4f0a, { 0xb6, 0x21, 0x5a, 0x0d, 0x14, 0x8e, 0x93, 0x01 } };
// On a RESOURCE: the engine's scene test called this a scene colour host.
const GUID IID_DuskSceneHost =
  { 0x9d3e7b11, 0x2c48, 0x4f0a, { 0xb6, 0x21, 0x5a, 0x0d, 0x14, 0x8e, 0x93, 0x02 } };
// ...93, 0x03 was a cached shader-resource view over the scene colour host,
// hung off the host with SetPrivateDataInterface. It is deliberately gone; see
// sourceViewFor for why that particular trick cannot be used here.
// On a CONTEXT: the back buffer is currently bound as a colour render target,
// which on this engine means the composite is being set up.
const GUID IID_DuskSsaaComposite =
  { 0x9d3e7b11, 0x2c48, 0x4f0a, { 0xb6, 0x21, 0x5a, 0x0d, 0x14, 0x8e, 0x93, 0x04 } };

void setMarker(ID3D11DeviceChild* on, const GUID& key) {
  const UINT value = 1;
  if (on)
    on->SetPrivateData(key, sizeof(value), &value);
}

void clearMarker(ID3D11DeviceChild* on, const GUID& key) {
  if (on)
    on->SetPrivateData(key, 0, nullptr);
}

bool hasMarker(ID3D11DeviceChild* on, const GUID& key) {
  UINT value = 0;
  UINT size = sizeof(value);
  return on && SUCCEEDED(on->GetPrivateData(key, &size, &value)) && value;
}

// ---- counters and one-shots ------------------------------------------------
//
// Every number here answers a question that would otherwise be answered by
// inference, which is how three of the four previous attempts shipped. The
// distinction the log has to preserve is CONFIGURED (this file's FIXES line and
// HIGHRES: scene size) / ATTACHED ('composite identified') / DOING SOMETHING
// ('engaged', and these counters growing).
std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_compositeBinds{0};
std::atomic<uint64_t> g_substitutions{0};
std::atomic<uint64_t> g_downscales{0};
std::atomic<uint64_t> g_passFailures{0};
std::atomic<uint64_t> g_backBufferRetags{0};
std::atomic<bool> g_backBufferKnown{false};
std::atomic<UINT> g_backWidth{0};
std::atomic<UINT> g_backHeight{0};

// The pass's own re-entry guard.
//
// The downscale runs INSIDE a PSSetShaderResources detour and then calls SMAA,
// which binds shader resources of its own through the public method. Without
// this, that bind would walk straight back into ssaaSubstituteShaderResources
// with the composite marker still set. Thread-local because the guard is about
// one call stack, not about the context.
thread_local bool g_inPass = false;

}  // namespace

// ---- the downscale pass ----------------------------------------------------

namespace {

using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
  const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
  ID3DBlob**, ID3DBlob**);

// Fullscreen triangle from SV_VertexID: no vertex buffer and no input layout,
// so the pass needs nothing of the game's IA state.
//
// The box filter is the Arland project's, unchanged. Its comment explains the
// half-ratio backoff, which is what makes odd ratios land on texel centres.
const char* kDownscaleHlsl = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}

cbuffer Params : register(b0) {
  float2 texel;    // 1 / render size
  float2 ratio;    // render size / display size, per axis
  float  samples;  // samples per axis
  float  sharpen;  // 0 = off
  float2 padding;
};

Texture2D    src  : register(t0);
SamplerState samp : register(s0);

// Average the source texels one output pixel actually covers.
//
// The footprint of output pixel p spans source texels [p*ratio, (p+1)*ratio].
// uv * sourceSize is the CENTRE of that footprint, so backing off half a ratio
// gives its top-left corner; samples are then spread evenly across it. For an
// integer ratio with samples == ratio this lands exactly on texel centres and
// the result is an exact box filter -- for odd ratios as well as even ones.
float3 boxAt(float2 uv) {
  int n = (int) samples;
  float2 sourceSize = 1.0 / texel;
  float2 origin = uv * sourceSize - 0.5 * ratio;
  float2 step = ratio / samples;
  float3 sum = 0.0;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      float2 position = origin + (float2(x, y) + 0.5) * step;
      sum += src.SampleLevel(samp, position * texel, 0).rgb;
    }
  }
  return sum / (samples * samples);
}

float4 PSMain(VSOut i) : SV_TARGET {
  float3 c = boxAt(i.uv);
  if (sharpen > 0.0) {
    // A box filter is an average, and an average is a blur -- so a correct
    // downscale is inherently softer than the image it came from. Sharpening
    // here rather than in a pass of its own costs four more box evaluations
    // and no extra bandwidth, because the taps are already in cache.
    float2 destTexel = ratio * texel;          // one output pixel, in source uv
    float3 l = boxAt(i.uv - float2(destTexel.x, 0.0));
    float3 r = boxAt(i.uv + float2(destTexel.x, 0.0));
    float3 u = boxAt(i.uv - float2(0.0, destTexel.y));
    float3 d = boxAt(i.uv + float2(0.0, destTexel.y));
    float3 mean = (l + r + u + d) * 0.25;
    float3 sharpened = c + (c - mean) * sharpen;
    // Clamped to the neighbourhood, which is what keeps this from ringing.
    // An unsharp mask left unclamped puts a bright halo on every dark edge and
    // reads as exactly the oversharpened look it is.
    float3 lo = min(min(min(l, r), min(u, d)), c);
    float3 hi = max(max(max(l, r), max(u, d)), c);
    c = clamp(sharpened, lo, hi);
  }
  return float4(c, 1.0);
}
)HLSL";

struct DownscaleParams {
  float texel[2];
  float ratio[2];
  float samples;
  float sharpen;
  float padding[2];
};

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11Texture2D* g_small = nullptr;
ID3D11RenderTargetView* g_smallRTV = nullptr;
ID3D11ShaderResourceView* g_smallSRV = nullptr;
UINT g_smallWidth = 0, g_smallHeight = 0;
DXGI_FORMAT g_smallFormat = DXGI_FORMAT_UNKNOWN;
bool g_passReady = false;
// Atomic because it is the one flag here read outside g_passBusy: the early-out
// at the top of ssaaSubstituteShaderResources tests it before the guard is
// claimed, and this engine records on deferred contexts of which it may hold
// several. Every write is still inside the guard.
std::atomic<bool> g_passBroken{false};

// WHAT g_small CURRENTLY HOLDS -- part of g_small's state, and declared with it
// so nothing can reallocate the texture without facing the question. See the
// note above ssaaSubstituteShaderResources' use of them for why one texture
// gets one occupant and a second host in the same frame is refused.
const void* g_smallHost = nullptr;
uint64_t g_smallFrame = 0;

// How many pipeline slots the state bracket below saves. Wider than the pass
// itself touches on purpose: SMAA runs inside this bracket and binds up to ten
// shader resources while restoring only four of them, so the four it would
// leave behind are covered here instead of by editing a validated feature.
constexpr UINT kSavedSrvs = 16;
constexpr UINT kSavedSamplers = 4;
constexpr UINT kSavedConstantBuffers = 4;

// Every hooked context method this pass needs, resolved to its trampoline once.
//
// This is the fix for the defect that hung attempt 4. The public methods are
// detoured -- OMSetRenderTargets into the scene/UI boundary check, Draw into
// the high-resolution raster correction, PSSetShaderResources into this very
// function -- and a pass that calls them is asking the mod to interpret its own
// internal state changes as the game's.
struct PassBinder {
  ID3D11DeviceContext* ctx;
  const ContextOriginals& originals;

  explicit PassBinder(ID3D11DeviceContext* c)
    : ctx(c), originals(d3d11OriginalsFor(c)) {}

  void targets(UINT count, ID3D11RenderTargetView* const* rtvs,
               ID3D11DepthStencilView* dsv) const {
    d3d11SetRenderTargets(ctx, count, rtvs, dsv);
  }
  void viewports(UINT count, const D3D11_VIEWPORT* vp) const {
    if (originals.rsSetViewports) originals.rsSetViewports(ctx, count, vp);
    else ctx->RSSetViewports(count, vp);
  }
  void scissors(UINT count, const D3D11_RECT* rects) const {
    if (originals.rsSetScissorRects) originals.rsSetScissorRects(ctx, count, rects);
    else ctx->RSSetScissorRects(count, rects);
  }
  void shaderResources(UINT start, UINT count,
                       ID3D11ShaderResourceView* const* srvs) const {
    if (originals.psSetShaderResources)
      originals.psSetShaderResources(ctx, start, count, srvs);
    else ctx->PSSetShaderResources(start, count, srvs);
  }
  void draw(UINT vertices, UINT start) const {
    if (originals.draw) originals.draw(ctx, vertices, start);
    else ctx->Draw(vertices, start);
  }
};

// Save and restore everything this pass disturbs. Same discipline as the SMAA
// pre-UI pass, and for the same reason: the engine is midway through building a
// frame -- in fact midway through setting up the composite's own shader
// resources -- and its next call expects to find its own state.
struct ScopedPassState {
  const PassBinder& bind;
  ID3D11DeviceContext* ctx;
  ID3D11InputLayout* layout = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11Buffer* vertexBuffer = nullptr;
  UINT vertexStride = 0, vertexOffset = 0;
  ID3D11RasterizerState* raster = nullptr;
  UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  ID3D11BlendState* blend = nullptr;
  FLOAT blendFactor[4] = {};
  UINT sampleMask = 0;
  ID3D11DepthStencilState* depthState = nullptr;
  UINT stencilRef = 0;
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  // The stages between the vertex and pixel shaders. This pass sets neither,
  // and that is exactly why they have to be saved and cleared: a fullscreen
  // triangle drawn with the engine's geometry or tessellation shaders still
  // bound is not a fullscreen triangle. SMAA's bracket omits them and has been
  // fine, but SMAA fires at the scene/UI boundary and this fires mid-composite
  // -- a point nothing has ever run at before, so "the engine happens to have
  // none bound here" is an assumption rather than a measurement.
  ID3D11GeometryShader* gs = nullptr;
  ID3D11HullShader* hs = nullptr;
  ID3D11DomainShader* ds = nullptr;
  ID3D11Buffer* vsCbs[kSavedConstantBuffers] = {};
  ID3D11Buffer* psCbs[kSavedConstantBuffers] = {};
  ID3D11SamplerState* samplers[kSavedSamplers] = {};
  ID3D11ShaderResourceView* srvs[kSavedSrvs] = {};

  explicit ScopedPassState(const PassBinder& b) : bind(b), ctx(b.ctx) {
    ctx->IAGetInputLayout(&layout);
    ctx->IAGetPrimitiveTopology(&topology);
    ctx->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
    ctx->RSGetState(&raster);
    ctx->RSGetViewports(&viewportCount, viewports);
    ctx->RSGetScissorRects(&scissorCount, scissors);
    ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
    ctx->OMGetDepthStencilState(&depthState, &stencilRef);
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->GSGetShader(&gs, nullptr, nullptr);
    ctx->HSGetShader(&hs, nullptr, nullptr);
    ctx->DSGetShader(&ds, nullptr, nullptr);
    ctx->VSGetConstantBuffers(0, kSavedConstantBuffers, vsCbs);
    ctx->PSGetConstantBuffers(0, kSavedConstantBuffers, psCbs);
    ctx->PSGetSamplers(0, kSavedSamplers, samplers);
    ctx->PSGetShaderResources(0, kSavedSrvs, srvs);
  }

  ~ScopedPassState() {
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(topology);
    ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
    ctx->RSSetState(raster);
    bind.viewports(viewportCount, viewports);
    bind.scissors(scissorCount, scissors);
    ctx->OMSetBlendState(blend, blendFactor, sampleMask);
    ctx->OMSetDepthStencilState(depthState, stencilRef);
    bind.targets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->GSSetShader(gs, nullptr, 0);
    ctx->HSSetShader(hs, nullptr, 0);
    ctx->DSSetShader(ds, nullptr, 0);
    ctx->VSSetConstantBuffers(0, kSavedConstantBuffers, vsCbs);
    ctx->PSSetConstantBuffers(0, kSavedConstantBuffers, psCbs);
    ctx->PSSetSamplers(0, kSavedSamplers, samplers);
    // Shader resources LAST, after the render targets, which is both the order
    // the engine itself established this state in and the order the validated
    // SMAA bracket restores in. It matters if any of these views reads a
    // surface that is also among the targets: D3D11 breaks that tie by dropping
    // whichever binding arrived first, and restoring in the engine's own order
    // reproduces the engine's own outcome rather than inventing a new one.
    bind.shaderResources(0, kSavedSrvs, srvs);
    release(layout); release(vertexBuffer); release(raster); release(blend);
    release(depthState); release(dsv); release(vs); release(ps);
    release(gs); release(hs); release(ds);
    for (auto*& b : vsCbs) release(b);
    for (auto*& b : psCbs) release(b);
    for (auto*& s : samplers) release(s);
    for (auto*& s : srvs) release(s);
    for (auto*& r : rtvs) release(r);
  }
};

bool compile(PFN_D3DCompile D3DCompile, const char* entry, const char* target,
             ID3DBlob** blob) {
  ID3DBlob* err = nullptr;
  const HRESULT hr = D3DCompile(kDownscaleHlsl, std::strlen(kDownscaleHlsl),
    "ssaa-downscale", nullptr, nullptr, entry, target, 0, 0, blob, &err);
  if (FAILED(hr)) {
    log("SSAA: compile failed entry=", entry, " hr=0x", std::hex,
        uint32_t(hr), std::dec,
        err ? " : " : "",
        err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    if (err) err->Release();
    return false;
  }
  if (err) err->Release();
  return true;
}

bool initPass(ID3D11Device* device) {
  if (g_passReady) return true;
  if (g_passBroken) return false;
  g_passBroken = true;   // cleared on success; one attempt only

  HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
  if (!compiler) compiler = LoadLibraryA("d3dcompiler.dll");
  if (!compiler) { log("SSAA: no d3dcompiler"); return false; }
  auto D3DCompile = reinterpret_cast<PFN_D3DCompile>(
    GetProcAddress(compiler, "D3DCompile"));
  if (!D3DCompile) { log("SSAA: no D3DCompile"); return false; }

  ID3DBlob* vs = nullptr;
  ID3DBlob* ps = nullptr;
  bool ok = compile(D3DCompile, "VSMain", "vs_4_0", &vs) &&
            compile(D3DCompile, "PSMain", "ps_4_0", &ps);
  if (ok)
    ok = SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),
           vs->GetBufferSize(), nullptr, &g_vs)) &&
         SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),
           ps->GetBufferSize(), nullptr, &g_ps));
  release(vs);
  release(ps);
  if (!ok) return false;

  D3D11_BUFFER_DESC cb = {};
  cb.ByteWidth = sizeof(DownscaleParams);
  cb.Usage = D3D11_USAGE_DYNAMIC;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&cb, nullptr, &g_cb))) return false;

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sd, &g_sampler))) return false;

  // Opaque, depth-less: the pass must not inherit whatever the engine had bound.
  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->CreateBlendState(&bd, &g_blend))) return false;

  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = FALSE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  if (FAILED(device->CreateDepthStencilState(&dd, &g_depth))) return false;

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  // Created through the device directly. Nothing hooks CreateRasterizerState
  // any more, but this pass draws one triangle into a texture it owns and has
  // no business inheriting whatever a future device-level rewrite decides.
  if (FAILED(device->CreateRasterizerState(&rd, &g_raster))) return false;

  g_passBroken = false;
  g_passReady = true;
  return true;
}

bool ensureSmall(ID3D11Device* device, UINT width, UINT height,
                 DXGI_FORMAT format) {
  // Format is part of the key, not just the size. Two hosts can want the same
  // destination size in different formats, and a texture that is the right size
  // in the wrong format is not a texture this pass can use: the render-target
  // view writes one format and the composite samples another.
  if (g_small && g_smallWidth == width && g_smallHeight == height &&
      g_smallFormat == format)
    return true;
  // A new texture holds nobody's downscale, whatever the old one held.
  g_smallHost = nullptr;
  g_smallFrame = 0;
  release(g_smallSRV);
  release(g_smallRTV);
  release(g_small);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = width; td.Height = height;
  td.MipLevels = 1; td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  // THROUGH THE ORIGINAL, never the hooked device. The high-resolution fix
  // rewrites any 1920x1080 target it sees into the scene size, because that is
  // the size the engine hard-codes its auxiliary targets at. This texture is the
  // mod's own, and at a display resolution of exactly 1920x1080 it is
  // indistinguishable from one of those -- so routing it through the hook had it
  // silently allocated at the scene size, and the downscale then wrote a
  // 1920x1080 image into the corner of a 2880x1620 texture the composite sampled
  // whole. That is the same rule the project already applies to context calls:
  // the mod's own D3D11 work must not travel through the mod's own hooks.
  if (FAILED(createTexture2DUnhooked(device, &td, nullptr, &g_small)) ||
      FAILED(device->CreateRenderTargetView(g_small, nullptr, &g_smallRTV)) ||
      FAILED(device->CreateShaderResourceView(g_small, nullptr, &g_smallSRV))) {
    release(g_smallSRV); release(g_smallRTV); release(g_small);
    g_smallWidth = g_smallHeight = 0;
    g_smallFormat = DXGI_FORMAT_UNKNOWN;
    return false;
  }
  g_smallWidth = width; g_smallHeight = height;
  g_smallFormat = format;
  return true;
}

// Samples per axis: enough to cover every source texel the output pixel spans,
// so a 3x ratio takes 3 and averages all nine rather than four corners of them.
// Rounded up, because covering slightly more than the footprint is a mild blur
// while covering less is aliasing.
float downscaleSamples(UINT sourceHeight, UINT destHeight) {
  if (!destHeight) return 1.0f;
  float samples = std::ceil(float(sourceHeight) / float(destHeight));
  if (samples < 1.0f) samples = 1.0f;
  if (samples > 8.0f) samples = 8.0f;
  return samples;
}

DXGI_FORMAT concreteFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
  }
}

// A shader-resource view over the scene colour host, created for this pass and
// released with it.
//
// NOT cached on the host with SetPrivateDataInterface, which is what an earlier
// version of this function did and what the removed multisample twins could
// safely do. That trick is a reference cycle here, and the difference is one
// word: a twin was a SEPARATE texture and held no reference back to the host,
// while a shader-resource view holds a strong reference to the resource it
// views. host -> (private data) -> view -> host is a cycle neither side can
// ever leave, so every scene colour target the engine ever allocated would be
// leaked for the life of the process -- a quarter of a gigabyte apiece at 4K
// and 200%, and a fresh set on every swap-chain resize.
//
// The cost of not caching is one CreateShaderResourceView per downscale, which
// is once per frame, against a pass that box-filters several million pixels
// with up to 64 taps each and then runs three SMAA passes over the result.
ID3D11ShaderResourceView* sourceViewFor(ID3D11Device* device,
                                        ID3D11Texture2D* host,
                                        const D3D11_TEXTURE2D_DESC& desc) {
  D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
  vd.Format = concreteFormat(desc.Format);
  vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  vd.Texture2D.MipLevels = 1;
  ID3D11ShaderResourceView* view = nullptr;
  if (FAILED(device->CreateShaderResourceView(host, &vd, &view)) || !view)
    return nullptr;
  return view;   // caller releases
}

// WHAT g_small CURRENTLY HOLDS. Not a memo of what has been downscaled -- a
// statement about the contents of one texture, which is what g_small is.
//
// An earlier version kept a four-entry table of (host, frame) pairs and treated
// a hit on any of them as "the result is ready", while every downscale wrote
// into the same g_small. That is only sound if at most one host is ever
// downscaled per frame. If the composite samples two different scene colour
// hosts -- and the ping-pong pair means there are two, and the DOF composite in
// the shader bundle declares more than one full-screen colour input -- the
// second downscale overwrites the first, and the slot already substituted for
// the first host silently starts reading the second one's image. The draw that
// consumes both happens after both, so the wrong picture is the guaranteed
// outcome rather than a risk. On a deferred context, where this engine records
// its scene, that ordering is not even a race: it is simply the replay order.
//
// So: one texture, one occupant, stated as such. A second host in the same
// frame is REFUSED rather than served a stale or overwritten image -- it keeps
// its full-size view, the engine's own bilinear resamples it, and the refusal
// is counted so the log says the composite has more than one scene input
// instead of leaving it to be inferred from a picture that looks wrong.
//
// Raw pointer, compared and never dereferenced, and only ever matched within
// the frame it was recorded in -- during which the host is alive by
// construction, because something is sampling it. Declared with g_small above.
std::atomic<uint64_t> g_secondHostRefusals{0};

// Concurrency guard, distinct from the re-entry guard.
//
// g_inPass is thread_local and answers "am I already inside the pass on THIS
// call stack", which is what stops SMAA's public binds from re-entering the
// substitution. It says nothing about a second thread. Everything the pass owns
// -- g_small and its views, the shaders, the constant buffer, g_smallHost -- is
// one shared set, and this engine records on deferred contexts of which it may
// hold several (d3d11_hooks.h). Two threads inside ensureSmall is a texture
// released while the other thread has it bound.
//
// Refuses rather than waits. A lock taken inside a D3D11 detour is a deadlock
// waiting for the right pair of threads; declining costs one frame of one
// scene target's sharpness and is counted.
std::atomic<bool> g_passBusy{false};
ID3D11Device* g_ownerDevice = nullptr;

// Runtime topology found one D3D11 device throughout both KTGL games. Make
// that measured ownership an enforced boundary: a recreated/second device is
// refused instead of being handed shaders, views, or textures allocated by the
// original device.
bool acceptsDevice(ID3D11Device* device) {
  if (!g_ownerDevice) {
    g_ownerDevice = device;
    g_ownerDevice->AddRef();
    return true;
  }
  if (g_ownerDevice == device)
    return true;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true, std::memory_order_relaxed))
    log("SSAA: a second D3D11 device reached the shared downscale pass;"
        " refusing it so resources are never bound across devices");
  return false;
}

// Box-filter `host` down to `destWidth` x `destHeight` into g_small, then run
// SMAA on the result if it is enabled. Returns false if anything failed, in
// which case nothing was substituted and the engine's own resample stands.
bool runDownscale(ID3D11DeviceContext* context, ID3D11Texture2D* host,
                  const D3D11_TEXTURE2D_DESC& sourceDesc,
                  UINT destWidth, UINT destHeight) {
  ID3D11Device* device = nullptr;
  host->GetDevice(&device);
  if (!device || !acceptsDevice(device)) {
    if (device)
      device->Release();
    g_passFailures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const DXGI_FORMAT format = concreteFormat(sourceDesc.Format);
  ID3D11ShaderResourceView* source = nullptr;
  // Which step failed, and the host it failed on. A count on its own says the
  // pass declined without saying what it declined, which cost a whole run to
  // find out the last time this fired.
  const char* step = "shaders";
  bool ok = initPass(device);
  if (ok) {
    step = "the destination texture";
    ok = ensureSmall(device, destWidth, destHeight, format);
  }
  if (ok) {
    step = "a view over the source";
    ok = (source = sourceViewFor(device, host, sourceDesc)) != nullptr;
  }
  if (!ok) {
    if (g_passFailures.fetch_add(1, std::memory_order_relaxed) == 0)
      log("SSAA: the downscale pass could not prepare ", step, " for a ",
          std::dec, sourceDesc.Width, "x", sourceDesc.Height, " host"
          " (format=", unsigned(sourceDesc.Format),
          " bind=0x", std::hex, sourceDesc.BindFlags, std::dec,
          "); the engine's own bilinear resample stands for it");
    release(source);
    device->Release();
    return false;
  }

  DownscaleParams params = {};
  params.texel[0] = 1.0f / float(sourceDesc.Width);
  params.texel[1] = 1.0f / float(sourceDesc.Height);
  params.ratio[0] = float(sourceDesc.Width) / float(destWidth);
  params.ratio[1] = float(sourceDesc.Height) / float(destHeight);
  params.samples = downscaleSamples(sourceDesc.Height, destHeight);
  params.sharpen = kDownscaleSharpen;
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT mapResult =
    context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (FAILED(mapResult)) {
    if (g_passFailures.fetch_add(1, std::memory_order_relaxed) == 0)
      log("SSAA: constant-buffer update failed (hr=0x", std::hex,
          uint32_t(mapResult), std::dec,
          "); the downscale was skipped rather than using stale parameters");
    release(source);
    device->Release();
    return false;
  }
  std::memcpy(mapped.pData, &params, sizeof(params));
  context->Unmap(g_cb, 0);

  const PassBinder bind(context);
  {
    // Bracketed: this fires in the middle of the composite's own setup, and the
    // composite expects to find the state it had.
    ScopedPassState saved(bind);

    const D3D11_VIEWPORT viewport = {
      0.0f, 0.0f, float(destWidth), float(destHeight), 0.0f, 1.0f };
    const D3D11_RECT scissor = { 0, 0, LONG(destWidth), LONG(destHeight) };
    ID3D11ShaderResourceView* none[kSavedSrvs] = {};
    bind.shaderResources(0, kSavedSrvs, none);
    bind.targets(1, &g_smallRTV, nullptr);
    bind.viewports(1, &viewport);
    bind.scissors(1, &scissor);
    context->RSSetState(g_raster);
    context->OMSetBlendState(g_blend, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(g_depth, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_vs, nullptr, 0);
    context->PSSetShader(g_ps, nullptr, 0);
    // Cleared, not left inherited: this triangle is generated from SV_VertexID
    // and must reach the rasteriser as it left the vertex shader. SMAA runs
    // inside this bracket and sets neither of them either, so clearing here
    // covers its draws as well.
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &g_cb);
    context->PSSetSamplers(0, 1, &g_sampler);
    bind.shaderResources(0, 1, &source);
    bind.draw(3, 0);
    bind.shaderResources(0, kSavedSrvs, none);

    // With pre-UI SMAA selected, it runs HERE on the downscaled result rather
    // than as its own injection on the supersampled scene target. The shared
    // entry point also owns the DUSK_SMAA_PREUI switch, so the diagnostic
    // Present-time mode remains authoritative on this route too.
    //
    // Three reasons, and the first is the one that matters. SMAA is a
    // morphological filter whose search distances are counted in pixels, so it
    // belongs at display resolution -- on a 2x scene target every edge is twice
    // as wide as its thresholds expect. Second, it is a quarter of the work at
    // 2560x1440 than at 5120x2880, including the full-surface copy it starts
    // with. Third, it happens inside a bracket this pass already holds, instead
    // of taking and restoring pipeline state a second time.
    //
    // Still pre-UI by construction: the composite has not drawn yet, and this
    // engine draws its interface onto the back buffer after the composite. That
    // is why scenePassNoteBoundary stands its own SMAA call down whenever
    // supersampling has engaged -- two pre-UI passes would be one too many,
    // and the one at the boundary is the one keyed on a transition that fires
    // 5 to 22 times a frame.
    smaaApplySceneColor(context, g_small);
    // And sharpening, for all three of the same reasons plus the order rule in
    // sharpen.h: after the antialiasing, never before. This is a SECOND sharpen
    // on the frame -- the resample above already folds one in -- and that is
    // deliberate: the two are different settings and the slider is meant to add
    // to what supersampling does rather than be absorbed by it.
    sharpenApply(context, g_small);
  }

  release(source);
  device->Release();
  g_downscales.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace

// ---- the interception points -----------------------------------------------

void ssaaNoteBackBuffer(IDXGISwapChain* swapChain) {
  if (!ssaaActive() || !swapChain)
    return;
  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_ID3D11Texture2D,
                                  reinterpret_cast<void**>(&back))) || !back) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
      log("SSAA: the swap chain would not hand over buffer 0, so the composite"
          " cannot be identified and supersampling will not engage");
    return;
  }
  setMarker(back, IID_DuskBackBuffer);
  {
    // Kept, not just logged. This is the only place the mod learns the size of
    // the thing it is ultimately drawing into, as opposed to the size the game
    // believes it is rendering at, and windowed mode is where those two come
    // apart. The Arland project reads the same descriptor for the same reason.
    D3D11_TEXTURE2D_DESC desc = {};
    back->GetDesc(&desc);
    g_backWidth.store(desc.Width, std::memory_order_relaxed);
    g_backHeight.store(desc.Height, std::memory_order_relaxed);
    if (!g_backBufferKnown.exchange(true, std::memory_order_relaxed))
      log("SSAA: back buffer identified ", std::dec, desc.Width, "x",
          desc.Height, " format=", uint32_t(desc.Format));
  }
  back->Release();
}

void ssaaTagSceneHost(ID3D11Texture2D* sceneColor) {
  if (!ssaaConfigured() || !sceneColor)
    return;
  setMarker(sceneColor, IID_DuskSceneHost);
}

bool ssaaIsSceneHost(ID3D11Texture2D* texture) {
  return texture && hasMarker(texture, IID_DuskSceneHost);
}

bool ssaaIsBackBuffer(ID3D11Texture2D* texture) {
  return texture && hasMarker(texture, IID_DuskBackBuffer);
}

void ssaaNoteTargetsBound(ID3D11DeviceContext* context, unsigned int numViews,
                          ID3D11RenderTargetView* const* views) {
  if (!ssaaActive() || !context)
    return;

  bool backBufferBound = false;
  if (views) {
    for (unsigned int i = 0; i < numViews && !backBufferBound; ++i) {
      if (!views[i])
        continue;
      ID3D11Resource* resource = nullptr;
      views[i]->GetResource(&resource);
      if (resource) {
        backBufferBound = hasMarker(resource, IID_DuskBackBuffer);
        resource->Release();
      }
    }
  }

  // Written only on a change: the marker is context private data, and a bind is
  // one of the hottest calls in the frame.
  const bool marked = hasMarker(context, IID_DuskSsaaComposite);
  if (backBufferBound == marked)
    return;
  if (backBufferBound) {
    setMarker(context, IID_DuskSsaaComposite);
    const uint64_t binds = g_compositeBinds.fetch_add(1, std::memory_order_relaxed);
    if (binds == 0)
      log("SSAA: composite identified -- bind whose colour target is the"
          " swap-chain back buffer (frame ", std::dec,
          g_frame.load(std::memory_order_relaxed), ")");
  } else {
    clearMarker(context, IID_DuskSsaaComposite);
  }
}

bool ssaaSubstituteShaderResources(ID3D11DeviceContext* context,
                                   unsigned int startSlot,
                                   unsigned int numViews,
                                   ID3D11ShaderResourceView* const* views,
                                   ID3D11ShaderResourceView** substituted,
                                   unsigned int capacity) {
  if (!ssaaActive() || g_inPass || g_passBroken || !context || !views ||
      !numViews || !substituted)
    return false;
  if (!hasMarker(context, IID_DuskSsaaComposite))
    return false;

  // The size the composite is about to sample the scene down to, and the two
  // routes answer it differently.
  //
  // On Ayesha it is the main render size: the engine composites into a back
  // buffer of that size and highResMainSize is the sole owner of the number.
  //
  // On the KTGL clamp route it is the SWAP CHAIN size, because that is what the
  // clamp forced the back buffer to and the main render size is never learned
  // there at all -- the high-resolution fix is unsupported on those games, so
  // highResMainSize would return false and this would decline every call.
  unsigned int destWidth = 0, destHeight = 0;
  const SsaaPolicy& policy = ssaaPolicy();
  if (!policy.substitutionDestSize(&destWidth, &destHeight) ||
      !destWidth || !destHeight)
    return false;

  // Which of these views reads a scene colour host that is genuinely larger
  // than the display. Anything else the composite samples -- the blur ladder,
  // the depth host, a lookup texture -- is left exactly as it arrived.
  //
  // The scene-host TAG is required on Ayesha and deliberately not on the clamp
  // route. That tag comes from the engine's scene-target test, and the KTGL
  // games have none. What stands in for it is stronger than it sounds rather
  // than weaker: this code only runs while the composite marker is set, which
  // means the swap chain's own back buffer is bound as a render target, and the
  // only thing being sampled at that moment which is larger than the back
  // buffer is the frame being resolved into it. That is a runtime fact about
  // this one call, not a guess about which surface is the scene.
  unsigned int slot = 0;
  ID3D11Texture2D* host = nullptr;
  D3D11_TEXTURE2D_DESC hostDesc = {};
  for (unsigned int i = 0; i < numViews && !host; ++i) {
    if (!views[i])
      continue;
    ID3D11Resource* resource = nullptr;
    views[i]->GetResource(&resource);
    if (!resource)
      continue;
    ID3D11Texture2D* texture = nullptr;
    if ((!policy.requiresSceneHostTag ||
         hasMarker(resource, IID_DuskSceneHost)) &&
        SUCCEEDED(resource->QueryInterface(
          IID_ID3D11Texture2D, reinterpret_cast<void**>(&texture))) && texture) {
      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      // A render target, and not a depth surface. Size alone is not enough on
      // the clamp route: the composite marker only says the back buffer is
      // bound, and this engine samples a 2048x2048 depth texture during that
      // window. It is larger than the display on both axes, so the size test
      // alone took it as the scene and tried to downscale it once per draw.
      // Nothing wrong reached the screen only because the pass could not build
      // a shader-resource view over a depth format, which is a failure standing
      // in for a rule. This is the rule: the scene colour host is a colour
      // target the engine renders into.
      const bool colourTarget =
        (desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 &&
        (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) == 0;
      if (colourTarget && desc.SampleDesc.Count == 1 &&
          desc.Width > destWidth && desc.Height > destHeight) {
        host = texture;          // ownership moves here
        hostDesc = desc;
        slot = i;
      } else {
        texture->Release();
      }
    }
    resource->Release();
  }
  if (!host)
    return false;

  // A composite that sets more slots than this array can hold is not something
  // to guess at: forward what the game asked for and say so once.
  if (numViews > capacity) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
      log("SSAA: the composite set ", std::dec, numViews, " shader resources at"
          " once, more than this substitution handles; the engine's own"
          " bilinear resample stands");
    host->Release();
    return false;
  }

  // The occupant check, possible downscale, tuple publication and returned
  // view handoff are one transaction. Reading the host/frame pair before this
  // guard used to be a data race, and returning g_smallSRV after dropping it
  // used to let another context resize/release the view before the caller
  // bound it.
  bool expected = false;
  if (!g_passBusy.compare_exchange_strong(expected, true,
                                           std::memory_order_acquire)) {
    host->Release();
    return false;
  }

  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  bool ready = g_smallHost == host && g_smallFrame == frame;
  if (!ready) {
    // g_small already holds a DIFFERENT host's downscale for this frame, and
    // that result is bound in a slot whose draw has not happened yet. Leave it
    // alone; see the note on g_smallHost.
    if (g_smallHost && g_smallFrame == frame) {
      if (g_secondHostRefusals.fetch_add(1, std::memory_order_relaxed) == 0)
        log("SSAA: the composite sampled a second scene colour host in one"
            " frame; it keeps the engine's own resample so the first one's"
            " downscale is not overwritten underneath it");
      g_passBusy.store(false, std::memory_order_release);
      host->Release();
      return false;
    }
    g_inPass = true;
    ready = runDownscale(context, host, hostDesc, destWidth, destHeight);
    g_inPass = false;
    if (ready) {
      g_smallHost = host;
      g_smallFrame = frame;
    }
  }
  if (!ready || !g_smallSRV) {
    g_passBusy.store(false, std::memory_order_release);
    host->Release();
    return false;
  }

  for (unsigned int i = 0; i < numViews; ++i)
    substituted[i] = views[i];
  // Retained across the guard boundary. The caller releases after
  // PSSetShaderResources has synchronously taken the context's own reference.
  g_smallSRV->AddRef();
  substituted[slot] = g_smallSRV;
  g_passBusy.store(false, std::memory_order_release);
  host->Release();

  if (g_substitutions.fetch_add(1, std::memory_order_relaxed) == 0)
    log("SSAA: engaged -- ", std::dec, hostDesc.Width, "x", hostDesc.Height,
        " -> ", destWidth, "x", destHeight,
        " substituted at the composite's sample (slot ", startSlot + slot, ")");
  return true;
}

void ssaaClearContextState(ID3D11DeviceContext* context) {
  if (!ssaaActive() || !context)
    return;
  clearMarker(context, IID_DuskSsaaComposite);
}

bool ssaaEngaged() {
  return g_substitutions.load(std::memory_order_relaxed) > 0;
}

// ---- the two engine routes ------------------------------------------------
//
// Which route this process takes is answered by supersample_policy.h, resolved
// lazily from the executable. Nothing below asks which engine it is running in.

bool ssaaActive() {
  return ssaaPolicy().routeActive();
}

// Every function present, every answer no. Returned by the dispatcher when no
// engine matched, and used by the Ayesha policy for the two clamp entries it has
// no use for -- so no caller has to test a pointer before calling.
const SsaaPolicy& ssaaNoPolicy() {
  static const SsaaPolicy policy = {
    [] { return false; },
    [] (unsigned int*, unsigned int*) { return false; },
    [] (ID3D11DeviceContext*, unsigned int*, unsigned int*) { return false; },
    [] (UINT*, UINT*, const char*) {},
    [] (const DXGI_SWAP_CHAIN_DESC*) {},
    false,
    false,
    false,
  };
  return policy;
}

bool ssaaBoundColorTargetSize(ID3D11DeviceContext* context, unsigned int* width,
                              unsigned int* height) {
  if (!context || !width || !height)
    return false;
  ID3D11RenderTargetView* rtv = nullptr;
  context->OMGetRenderTargets(1, &rtv, nullptr);
  if (!rtv) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
      log("SSAA: the composite draw has no colour target bound, so the"
          " viewport cannot be checked against it");
    return false;
  }
  ID3D11Resource* resource = nullptr;
  rtv->GetResource(&resource);
  rtv->Release();
  if (!resource)
    return false;
  unsigned int foundWidth = 0, foundHeight = 0;
  ID3D11Texture2D* texture = nullptr;
  if (SUCCEEDED(resource->QueryInterface(IID_ID3D11Texture2D,
                                         reinterpret_cast<void**>(&texture)))
      && texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    foundWidth = desc.Width;
    foundHeight = desc.Height;
    texture->Release();
  }
  resource->Release();
  if (!foundWidth || !foundHeight)
    return false;
  *width = foundWidth;
  *height = foundHeight;
  return true;
}

void ssaaCorrectCompositeViewport(ID3D11DeviceContext* context) {
  // THE HALF THE SUBSTITUTION DOES NOT COVER, and the first run without it
  // looked exactly like a feature that was not running at all.
  //
  // Substituting the sampled texture makes the composite read a correctly
  // downscaled image. It does not change where that image is drawn. The engine
  // sets its viewport from the size it believes it is rendering at -- the
  // multiplied one -- so the composite covers 3840x2160 of a 2560x1440 back
  // buffer and the player sees the top-left crop, sharp and wrongly framed.
  // The resample was right in the very first run; the rectangle was not.
  //
  // Corrected at DRAW time rather than when the viewport is set, because the
  // engine sets viewports and render targets in whichever order it likes and
  // only at the draw are both certainly current. Clamping every oversized
  // viewport instead would be wrong in the obvious way: the scene genuinely is
  // 3840x2160 and its own passes need a viewport that size. This fires only
  // while the swap chain's back buffer is the bound colour target.
  if (!ssaaActive() || !context)
    return;
  if (!hasMarker(context, IID_DuskSsaaComposite))
    return;

  // WHERE THE DESTINATION SIZE COMES FROM, and the two engines disagree for a
  // reason -- so the answer comes from the policy rather than from a branch
  // here. Ayesha reads the colour target that is actually bound; KTGL cannot,
  // because the clamp resized the back buffer behind the engine's back. See
  // supersample_policy.h.
  unsigned int destWidth = 0, destHeight = 0;
  if (!ssaaPolicy().compositeViewportSize(context, &destWidth, &destHeight))
    return;

  UINT count = 1;
  D3D11_VIEWPORT viewport = {};
  context->RSGetViewports(&count, &viewport);
  if (!count)
    return;
  // Exact match rather than "not too big". The clamp route only ever sees a
  // viewport that is too LARGE, and a `<=` test was right for it. Ayesha
  // windowed shows the opposite: the composite is handed a viewport smaller
  // than the back buffer and the rest of the window is never painted. One test
  // covers both, and a viewport that is already correct still costs nothing.
  const float observedWidth = viewport.Width;
  const float observedHeight = viewport.Height;
  // Printed on the first composite draw whatever happens next, including when
  // nothing needs correcting. Two builds in a row have now turned on a guess
  // about what these three numbers are, so they get stated instead.
  {
    static std::atomic<bool> stated{false};
    if (!stated.exchange(true, std::memory_order_relaxed))
      log("SSAA: first composite draw -- target ", std::dec, destWidth, "x",
          destHeight, ", viewport ", unsigned(observedWidth), "x",
          unsigned(observedHeight), " at ", int(viewport.TopLeftX), ",",
          int(viewport.TopLeftY));
  }
  if (viewport.Width == float(destWidth) &&
      viewport.Height == float(destHeight) &&
      viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f)
    return;

  viewport.Width = float(destWidth);
  viewport.Height = float(destHeight);
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  // Through the trampoline, never the public method: the latter re-enters this
  // module's own viewport detour.
  const ContextOriginals& originals = d3d11OriginalsFor(context);
  if (originals.rsSetViewports)
    originals.rsSetViewports(context, 1, &viewport);

  static std::atomic<bool> announced{false};
  if (!announced.exchange(true, std::memory_order_relaxed))
    log("SSAA: composite viewport corrected from ", std::dec,
        unsigned(observedWidth), "x", unsigned(observedHeight), " to ",
        destWidth, "x", destHeight,
        observedWidth > float(destWidth) || observedHeight > float(destHeight)
          ? " (the engine had it at the render size, which drew the scene"
            " cropped rather than scaled)"
          : " (the engine had it smaller than the back buffer, which left part"
            " of the window unpainted)");
}

void ssaaFitOutputWindow(const DXGI_SWAP_CHAIN_DESC* desc) {
  if (!ssaaActive() || !desc || !desc->OutputWindow)
    return;

  // Reported on BOTH routes and before anything is decided, because "the window
  // is not the size of the back buffer" is the one fact that separates a
  // composite drawn wrongly from a window that was never the right size, and
  // guessing between those two has already cost a round trip.
  RECT observed = {};
  if (GetClientRect(desc->OutputWindow, &observed)) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
      log("SSAA: output window client area ", std::dec,
          observed.right - observed.left, "x", observed.bottom - observed.top,
          ", back buffer ", desc->BufferDesc.Width, "x",
          desc->BufferDesc.Height,
          desc->Windowed ? " (windowed)" : " (fullscreen)");
  }

  // The resize itself belongs to whichever engine needs one. Ayesha's policy
  // supplies a no-op here; KTGL's sizes the window to the clamped client area.
  ssaaPolicy().fitOutputWindow(desc);
}

void ssaaClampPresentSize(UINT* width, UINT* height, const char* where) {
  ssaaPolicy().clampPresentSize(width, height, where);
}


void ssaaFrameTick(IDXGISwapChain* swapChain) {
  if (!ssaaActive())
    return;
  const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

  // CONFIGURED, once, and with the sizes in it. Deferred to the first frame
  // rather than logged at install because neither size exists at install time:
  // Ayesha creates its device before its swap chain, and the main render size
  // is learned from the first depth target after that.
  static std::atomic<bool> announced{false};
  if (!announced.load(std::memory_order_relaxed)) {
    unsigned int mainWidth = 0, mainHeight = 0;
    unsigned int sceneWidth = 0, sceneHeight = 0;
    if (highResMainSize(&mainWidth, &mainHeight) &&
        highResSceneSize(&sceneWidth, &sceneHeight) &&
        !announced.exchange(true, std::memory_order_relaxed))
      log("FIXES ssaa=", std::dec, ssaaPercent(), "% scene=", sceneWidth, "x",
          sceneHeight, " display=", mainWidth, "x", mainHeight,
          " sharpen=", int(kDownscaleSharpen * 100.0f + 0.5f),
          "% ('SSAA: composite identified' confirms attachment)");
  }

  // The one state in which everything below is wasted effort: without the
  // high-resolution fix nothing ever learns a main render size, so the scene
  // targets keep their hard-coded 1920x1080 and there is nothing enlarged to
  // downscale. Said out loud, because the alternative is a log full of zeroes.
  // Not on the clamp route, where the engine does the enlarging itself from its
  // own ini and the high-resolution fix is correctly unsupported. Saying
  // "supersampling is inactive" there would be the opposite of true, and it was
  // printed three lines above a line reporting the feature engaged.
  static std::atomic<bool> highResWarned{false};
  if (ssaaPolicy().requiresHighRes &&
      !featureEnabled(Feature::HighResRendering) &&
      !highResWarned.exchange(true, std::memory_order_relaxed))
    log("SSAA: high-resolution fix is off; supersampling requires it and is"
        " inactive");

  // Risk 4: the tag lives on the resource, and ResizeBuffers replaces it. One
  // GetBuffer per frame is cheap, and re-tagging is the whole repair.
  if (swapChain) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_ID3D11Texture2D,
                                       reinterpret_cast<void**>(&back))) &&
        back) {
      if (!hasMarker(back, IID_DuskBackBuffer)) {
        setMarker(back, IID_DuskBackBuffer);
        if (g_backBufferRetags.fetch_add(1, std::memory_order_relaxed) == 0)
          log("SSAA: the back buffer lost its tag and was re-identified"
              " (a swap-chain resize); composite identification continues");
      }
      back->Release();
    }
  }

  const uint64_t binds = g_compositeBinds.load(std::memory_order_relaxed);
  const uint64_t subs = g_substitutions.load(std::memory_order_relaxed);

  // The two silences worth naming, each once, after long enough that startup
  // cannot explain either.
  if (frame == 1800) {
    if (!binds)
      log("SSAA: configured but the composite was never identified after ",
          std::dec, frame, " frames -- no bind ever carried the swap-chain back"
          " buffer as a colour target. The scene is being resampled by the"
          " engine's own bilinear filter instead (soft, but correct)."
          " backBufferIdentified=",
          g_backBufferKnown.load(std::memory_order_relaxed) ? 1 : 0);
    else if (!subs)
      log("SSAA: composite identified but no scene target was ever sampled"
          " during it after ", std::dec, frame, " frames (compositeBinds=",
          binds, ") -- the composite sets its shader resources before it binds"
          " the back buffer, so the substitution never sees them");
  }

  if (frame % 600 == 0)
    log("SSAA compositeBinds=", std::dec, binds,
        " sceneSrvSubstitutions=", subs,
        " downscales=", g_downscales.load(std::memory_order_relaxed),
        " passFailures=", g_passFailures.load(std::memory_order_relaxed),
        // Non-zero means the composite has more than one scene colour input and
        // only the first is being downscaled. Not a malfunction -- the rest keep
        // the engine's own resample -- but it is the measurement that would say
        // a per-host destination texture is worth building.
        " secondHostRefusals=",
        g_secondHostRefusals.load(std::memory_order_relaxed));
}

}  // namespace atfix
