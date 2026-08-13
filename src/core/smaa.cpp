// SPDX-License-Identifier: MIT
//
// SMAA (Enhanced Subpixel Morphological Anti-Aliasing, Jimenez et al.) as a
// post-process over the finished frame. These games ship no antialiasing of any
// kind, and SMAA works on the finished image, so it smooths every visible edge
// regardless of how it was produced -- texture-interior and alpha-test edges
// included, which is exactly what multisampling cannot reach.
//
// Ported from the Arland project's src/smaa.cpp, which is this project's own
// code (MIT). The reference shader and the AreaTex/SearchTex lookup tables are
// vendored unchanged under vendor/smaa/ (Jimenez, Echevarria, Masia, Navarro,
// Gutierrez; MIT) and compiled at runtime through d3dcompiler, which is the
// same arrangement Arland uses.
//
// Each engine supplies a measured pre-UI boundary so the passes run over the
// scene before the interface is composited. Present remains a fallback when a
// pre-UI pass cannot run.
// Atelier Graphics Tweak also ships SMAA for these games, and confirmed two
// useful facts by inspection: the same MIT reference shader at
// SMAA_PRESET_ULTRA, and an injection point on the DEFERRED context. None of
// its code is used here -- it is unlicensed, and this is a port of ours.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "d3d11_hooks.h"
#include "smaa.h"
#include "smaa_shader.h"                  // kSmaaReferenceHlsl
#include "../../vendor/smaa/AreaTex.h"    // areaTexBytes, AREATEX_*
#include "../../vendor/smaa/SearchTex.h"  // searchTexBytes, SEARCHTEX_*
#include "game.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
  const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
  ID3DBlob**, ID3DBlob**);

// Matches the wrapper's SMAAShaderConstants below.
struct SmaaConstants {
  float subsampleIndices[4] = {0, 0, 0, 0};
  float rtMetrics[4] = {0, 0, 0, 0};   // 1/w, 1/h, w, h
  float blendFactor = 1.0f;
  float threshold = 0.0f;
  float maxSearchSteps = 0.0f;
  float maxSearchStepsDiag = 0.0f;
  float cornerRounding = 0.0f;
  float padding[3] = {0, 0, 0};
};

// Thin entry-point wrappers around the reference SMAA functions, appended after
// the reference shader in the compile unit. This is the Arland project's own
// wrapper, not the SMAA distribution's DX10 sample and not AGT's.
//
// Registers: s0 linear, s1 point; t0 area, t1 search, t2 colour, t3 colour
// (gamma), t8 edges, t9 blend; b0 the constants.
const char* kSmaaWrapper = R"WRAP(
Texture2D areaTex   : register(t0);
Texture2D searchTex : register(t1);
Texture2D colorTex  : register(t2);
Texture2D colorTexGamma : register(t3);
Texture2D edgesTex  : register(t8);
Texture2D blendTex  : register(t9);

void EdgeVS(float4 position : POSITION,
            out float4 svPosition : SV_POSITION,
            inout float2 texcoord : TEXCOORD0,
            out float4 offset[3] : TEXCOORD1) {
  svPosition = position;
  SMAAEdgeDetectionVS(texcoord, offset);
}
float4 EdgePS(float4 position : SV_POSITION,
              float2 texcoord : TEXCOORD0,
              float4 offset[3] : TEXCOORD1) : SV_TARGET {
  return float4(SMAAColorEdgeDetectionPS(texcoord, offset, colorTexGamma), 0.0, 0.0);
}

void WeightVS(float4 position : POSITION,
              out float4 svPosition : SV_POSITION,
              inout float2 texcoord : TEXCOORD0,
              out float2 pixcoord : TEXCOORD1,
              out float4 offset[3] : TEXCOORD2) {
  svPosition = position;
  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
}
float4 WeightPS(float4 position : SV_POSITION,
                float2 texcoord : TEXCOORD0,
                float2 pixcoord : TEXCOORD1,
                float4 offset[3] : TEXCOORD2) : SV_TARGET {
  return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset,
    edgesTex, areaTex, searchTex, g_SMAA.subsampleIndices);
}

void BlendVS(float4 position : POSITION,
             out float4 svPosition : SV_POSITION,
             inout float2 texcoord : TEXCOORD0,
             out float4 offset : TEXCOORD1) {
  svPosition = position;
  SMAANeighborhoodBlendingVS(texcoord, offset);
}
float4 BlendPS(float4 position : SV_POSITION,
               float2 texcoord : TEXCOORD0,
               float4 offset : TEXCOORD1) : SV_TARGET {
  return SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex);
}
)WRAP";

// SMAA.hlsl (HLSL_4 path) declares LinearSampler/PointSampler itself, so this
// does not; they auto-assign to s0/s1 in declaration order, which the sampler
// binding below matches.
const char* kSmaaPrefix =
  "#define SMAA_HLSL_4_1 1\n"
  "#define SMAA_PRESET_ULTRA 1\n"
  "struct SMAAShaderConstants { float4 subsampleIndices; float4 rt_metrics;"
  " float blendFactor; float threshld; float maxSearchSteps;"
  " float maxSearchStepsDiag; float cornerRounding;"
  " float padding0; float padding1; float padding2; };\n"
  "cbuffer SMAAGlobals : register(b0) { SMAAShaderConstants g_SMAA; }\n"
  "#define SMAA_RT_METRICS g_SMAA.rt_metrics\n";

// ---- state -----------------------------------------------------------------
std::atomic<bool> g_init{false};
bool g_broken = false;
std::atomic<bool> g_preUiProven{false};
std::atomic<bool> g_doneThisFrame{false};
// One resource tuple, used from both deferred pre-UI recording and the
// immediate Present fallback. Refuse an overlap instead of waiting inside a
// D3D hook; the next frame can try again without risking a graphics-lock
// deadlock. Runtime topology shows one game device per process, so a different
// device is rejected explicitly rather than receiving resources created by the
// first one.
std::atomic<bool> g_passBusy{false};
ID3D11Device* g_ownerDevice = nullptr;
UINT g_width = 0, g_height = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;

ID3D11VertexShader* g_edgeVS = nullptr;
ID3D11VertexShader* g_weightVS = nullptr;
ID3D11VertexShader* g_blendVS = nullptr;
ID3D11PixelShader*  g_edgePS = nullptr;
ID3D11PixelShader*  g_weightPS = nullptr;
ID3D11PixelShader*  g_blendPS = nullptr;

ID3D11InputLayout*  g_layout = nullptr;
ID3D11Buffer*       g_quad = nullptr;
ID3D11Buffer*       g_cb = nullptr;
ID3D11SamplerState* g_linear = nullptr;
ID3D11SamplerState* g_point = nullptr;
ID3D11BlendState*   g_blendState = nullptr;
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState*   g_raster = nullptr;
ID3D11ShaderResourceView* g_areaSRV = nullptr;
ID3D11ShaderResourceView* g_searchSRV = nullptr;

// Per-target targets, recreated if the incoming size or concrete format
// changes.
ID3D11Texture2D* g_sceneTex = nullptr;
ID3D11ShaderResourceView* g_sceneSRV = nullptr;
ID3D11Texture2D* g_edgesTex = nullptr;
ID3D11RenderTargetView* g_edgesRTV = nullptr;
ID3D11ShaderResourceView* g_edgesSRV = nullptr;
ID3D11Texture2D* g_weightTex = nullptr;
ID3D11RenderTargetView* g_weightRTV = nullptr;
ID3D11ShaderResourceView* g_weightSRV = nullptr;

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

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
    log("SMAA: a second D3D11 device reached the shared pass; refusing it so"
        " resources from the first device are never bound across devices");
  return false;
}

bool compile(PFN_D3DCompile D3DCompile, const char* entry, const char* target,
             ID3DBlob** blob) {
  std::string src;
  src.reserve(sizeof(kSmaaReferenceHlsl) + 4096);
  src += kSmaaPrefix;
  src += kSmaaReferenceHlsl;
  src += kSmaaWrapper;
  ID3DBlob* err = nullptr;
  const HRESULT hr = D3DCompile(src.data(), src.size(), "smaa", nullptr,
    nullptr, entry, target, 0, 0, blob, &err);
  if (FAILED(hr)) {
    log("SMAA: compile failed entry=", entry, " hr=0x", std::hex, hr,
      std::dec, err ? " : " : "",
      err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    if (err) err->Release();
    return false;
  }
  if (err) err->Release();
  return true;
}

bool initShared(ID3D11Device* dev) {
  HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
  if (!comp) comp = LoadLibraryA("d3dcompiler.dll");
  if (!comp) { log("SMAA: no d3dcompiler"); return false; }
  auto D3DCompile = reinterpret_cast<PFN_D3DCompile>(
    GetProcAddress(comp, "D3DCompile"));
  if (!D3DCompile) { log("SMAA: no D3DCompile"); return false; }

  ID3DBlob* evs = nullptr; ID3DBlob* wvs = nullptr; ID3DBlob* bvs = nullptr;
  ID3DBlob* eps = nullptr; ID3DBlob* wps = nullptr; ID3DBlob* bps = nullptr;
  bool ok = compile(D3DCompile, "EdgeVS", "vs_4_1", &evs) &&
    compile(D3DCompile, "WeightVS", "vs_4_1", &wvs) &&
    compile(D3DCompile, "BlendVS", "vs_4_1", &bvs) &&
    compile(D3DCompile, "EdgePS", "ps_4_1", &eps) &&
    compile(D3DCompile, "WeightPS", "ps_4_1", &wps) &&
    compile(D3DCompile, "BlendPS", "ps_4_1", &bps);
  if (ok)
    ok = SUCCEEDED(dev->CreateVertexShader(evs->GetBufferPointer(),
          evs->GetBufferSize(), nullptr, &g_edgeVS)) &&
      SUCCEEDED(dev->CreateVertexShader(wvs->GetBufferPointer(),
          wvs->GetBufferSize(), nullptr, &g_weightVS)) &&
      SUCCEEDED(dev->CreateVertexShader(bvs->GetBufferPointer(),
          bvs->GetBufferSize(), nullptr, &g_blendVS)) &&
      SUCCEEDED(dev->CreatePixelShader(eps->GetBufferPointer(),
          eps->GetBufferSize(), nullptr, &g_edgePS)) &&
      SUCCEEDED(dev->CreatePixelShader(wps->GetBufferPointer(),
          wps->GetBufferSize(), nullptr, &g_weightPS)) &&
      SUCCEEDED(dev->CreatePixelShader(bps->GetBufferPointer(),
          bps->GetBufferSize(), nullptr, &g_blendPS));

  // Fullscreen quad (clip xy + uv); the input layout comes from the edge VS.
  if (ok) {
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
        D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
        D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ok = SUCCEEDED(dev->CreateInputLayout(elems, 2, evs->GetBufferPointer(),
      evs->GetBufferSize(), &g_layout));
  }
  release(evs); release(wvs); release(bvs);
  release(eps); release(wps); release(bps);
  if (!ok) { log("SMAA: shader/layout init failed"); return false; }

  const float quad[] = {
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
  };
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(quad);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA qd = {}; qd.pSysMem = quad;
  ok = SUCCEEDED(dev->CreateBuffer(&bd, &qd, &g_quad));

  D3D11_BUFFER_DESC cbd = {};
  cbd.ByteWidth = sizeof(SmaaConstants);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ok = ok && SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_cb));

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_linear));
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_point));

  D3D11_BLEND_DESC bl = {};
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g_blendState));
  D3D11_DEPTH_STENCIL_DESC ds = {};
  ds.DepthEnable = FALSE;
  ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &g_depthState));
  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE;
  ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rs, &g_raster));

  // The precomputed lookup textures.
  D3D11_TEXTURE2D_DESC atd = {};
  atd.Width = AREATEX_WIDTH; atd.Height = AREATEX_HEIGHT; atd.MipLevels = 1;
  atd.ArraySize = 1; atd.Format = DXGI_FORMAT_R8G8_UNORM;
  atd.SampleDesc.Count = 1; atd.Usage = D3D11_USAGE_IMMUTABLE;
  atd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA ad = {};
  ad.pSysMem = areaTexBytes; ad.SysMemPitch = AREATEX_PITCH;
  ID3D11Texture2D* area = nullptr;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&atd, &ad, &area));
  if (ok) ok = SUCCEEDED(dev->CreateShaderResourceView(area, nullptr, &g_areaSRV));
  release(area);

  D3D11_TEXTURE2D_DESC std_ = {};
  std_.Width = SEARCHTEX_WIDTH; std_.Height = SEARCHTEX_HEIGHT;
  std_.MipLevels = 1; std_.ArraySize = 1; std_.Format = DXGI_FORMAT_R8_UNORM;
  std_.SampleDesc.Count = 1; std_.Usage = D3D11_USAGE_IMMUTABLE;
  std_.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sdd = {};
  sdd.pSysMem = searchTexBytes; sdd.SysMemPitch = SEARCHTEX_PITCH;
  ID3D11Texture2D* search = nullptr;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&std_, &sdd, &search));
  if (ok) ok = SUCCEEDED(dev->CreateShaderResourceView(search, nullptr, &g_searchSRV));
  release(search);

  if (!ok) log("SMAA: shared resource init failed");
  return ok;
}

void releaseSized() {
  release(g_sceneSRV); release(g_sceneTex);
  release(g_edgesSRV); release(g_edgesRTV); release(g_edgesTex);
  release(g_weightSRV); release(g_weightRTV); release(g_weightTex);
  g_width = g_height = 0;
  g_format = DXGI_FORMAT_UNKNOWN;
}

// The scene target is TYPELESS on this engine, which is how it carries both a
// render-target and a shader-resource view over one allocation. Every view this
// module creates needs a concrete format instead, and CopyResource is happy to
// move between the two as long as they are the same family.
DXGI_FORMAT concreteFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return format;
  }
}

// Save, and put back, every piece of pipeline state these passes touch.
//
// The Present-time path needs none of this: it runs after the game has finished
// submitting, so there is nothing of the game's to preserve. The pre-UI path is
// the opposite -- it fires in the middle of a frame the engine is still
// building, and anything left dirty lands on the UI draws that follow.
//
// Scissor rects are included, which the Arland implementation's equivalent
// omits. Nothing has gone wrong there because its games do not scissor at the
// injection point; that is a property of those games rather than a reason the
// state does not need restoring.
struct ScopedSmaaState {
  ID3D11DeviceContext* ctx;
  ID3D11InputLayout* layout = nullptr;
  ID3D11Buffer* vb = nullptr;
  UINT vbStride = 0, vbOffset = 0;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
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
  ID3D11Buffer* vsCb = nullptr;
  ID3D11Buffer* psCb = nullptr;
  ID3D11SamplerState* samplers[2] = {};
  ID3D11ShaderResourceView* srvs[10] = {};

  explicit ScopedSmaaState(ID3D11DeviceContext* c) : ctx(c) {
    ctx->IAGetInputLayout(&layout);
    ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
    ctx->IAGetPrimitiveTopology(&topology);
    ctx->RSGetState(&raster);
    ctx->RSGetViewports(&viewportCount, viewports);
    ctx->RSGetScissorRects(&scissorCount, scissors);
    ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
    ctx->OMGetDepthStencilState(&depthState, &stencilRef);
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->VSGetConstantBuffers(0, 1, &vsCb);
    ctx->PSGetConstantBuffers(0, 1, &psCb);
    ctx->PSGetSamplers(0, 2, samplers);
    ctx->PSGetShaderResources(0, 10, srvs);
  }

  ~ScopedSmaaState() {
    ctx->IASetInputLayout(layout);
    ctx->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
    ctx->IASetPrimitiveTopology(topology);
    ctx->RSSetState(raster);
    ctx->RSSetViewports(viewportCount, viewports);
    ctx->RSSetScissorRects(scissorCount, scissors);
    ctx->OMSetBlendState(blend, blendFactor, sampleMask);
    ctx->OMSetDepthStencilState(depthState, stencilRef);
    // Through the trampoline, not the public method: the latter is hooked, and
    // restoring state should not look like the game binding targets.
    d3d11SetRenderTargets(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs,
                          dsv);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &vsCb);
    ctx->PSSetConstantBuffers(0, 1, &psCb);
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->PSSetShaderResources(0, 10, srvs);
    release(layout); release(vb); release(raster); release(blend);
    release(depthState); release(dsv); release(vs); release(ps);
    release(vsCb); release(psCb);
    for (auto* s : samplers) release(s);
    for (auto* s : srvs) release(s);
    for (auto* r : rtvs) release(r);
  }
};

bool initSized(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt) {
  releaseSized();
  auto make = [&](DXGI_FORMAT format, UINT bind, ID3D11Texture2D** tex,
                  ID3D11RenderTargetView** rtv, ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = format; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
    // Through the original: see the same call in supersample.cpp. A pass
    // target that happens to be 1920x1080 would otherwise be rewritten to
    // the scene size by the high-resolution fix.
    if (FAILED(createTexture2DUnhooked(dev, &td, nullptr, tex)))
      return false;
    if (srv && FAILED(dev->CreateShaderResourceView(*tex, nullptr, srv)))
      return false;
    if (rtv && FAILED(dev->CreateRenderTargetView(*tex, nullptr, rtv)))
      return false;
    return true;
  };
  const bool ok =
    make(fmt, D3D11_BIND_SHADER_RESOURCE, &g_sceneTex, nullptr, &g_sceneSRV) &&
    make(DXGI_FORMAT_R8G8B8A8_UNORM,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
      &g_edgesTex, &g_edgesRTV, &g_edgesSRV) &&
    make(DXGI_FORMAT_R8G8B8A8_UNORM,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
      &g_weightTex, &g_weightRTV, &g_weightSRV);
  if (!ok) {
    log("SMAA: size ", std::dec, w, "x", h, " target init failed");
    releaseSized();
  } else {
    g_width = w;
    g_height = h;
    g_format = fmt;
  }
  return ok;
}

// The three passes, in place on `color`. `colorRTV` is a render-target view of
// it. Returns false on any setup failure.
bool smaaRunPasses(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                   ID3D11Texture2D* color, ID3D11RenderTargetView* colorRTV) {
  D3D11_TEXTURE2D_DESC cd = {};
  color->GetDesc(&cd);
  const DXGI_FORMAT viewFormat = concreteFormat(cd.Format);
  if (cd.SampleDesc.Count != 1)
    return false;

  if (!g_init.exchange(true)) {
    if (!initShared(dev)) g_broken = true;
    else log("FIXES smaa=active size=", std::dec, cd.Width, "x", cd.Height,
      " (Ultra preset)");
  }
  if (g_broken)
    return false;
  if (cd.Width != g_width || cd.Height != g_height ||
      viewFormat != g_format)
    if (!initSized(dev, cd.Width, cd.Height, viewFormat)) {
      g_broken = true;
      return false;
    }

  ctx->CopyResource(g_sceneTex, color);

  SmaaConstants cb;
  cb.rtMetrics[0] = 1.0f / float(cd.Width);
  cb.rtMetrics[1] = 1.0f / float(cd.Height);
  cb.rtMetrics[2] = float(cd.Width);
  cb.rtMetrics[3] = float(cd.Height);
  D3D11_MAPPED_SUBRESOURCE map = {};
  const HRESULT mapResult =
    ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
  if (FAILED(mapResult)) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
      log("SMAA: constant-buffer update failed (hr=0x", std::hex,
          uint32_t(mapResult), std::dec, "); pass skipped");
    return false;
  }
  std::memcpy(map.pData, &cb, sizeof(cb));
  ctx->Unmap(g_cb, 0);

  const UINT stride = 16, offset = 0;
  const float black[4] = {0, 0, 0, 0};
  D3D11_VIEWPORT vp = {0, 0, float(cd.Width), float(cd.Height), 0, 1};
  ctx->IASetInputLayout(g_layout);
  ctx->IASetVertexBuffers(0, 1, &g_quad, &stride, &offset);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(g_raster);
  ctx->OMSetBlendState(g_blendState, nullptr, 0xffffffff);
  ctx->OMSetDepthStencilState(g_depthState, 0);
  ID3D11SamplerState* samplers[2] = {g_linear, g_point};
  ctx->PSSetSamplers(0, 2, samplers);
  ctx->VSSetConstantBuffers(0, 1, &g_cb);
  ctx->PSSetConstantBuffers(0, 1, &g_cb);
  ID3D11ShaderResourceView* nullSRV[10] = {};

  // Pass 1: edge detection (colour at t3) -> edges.
  ctx->PSSetShaderResources(0, 10, nullSRV);
  ctx->ClearRenderTargetView(g_edgesRTV, black);
  d3d11SetRenderTargets(ctx, 1, &g_edgesRTV, nullptr);
  ID3D11ShaderResourceView* p1[4] = {g_areaSRV, g_searchSRV, g_sceneSRV,
    g_sceneSRV};
  ctx->PSSetShaderResources(0, 4, p1);
  ctx->VSSetShader(g_edgeVS, nullptr, 0);
  ctx->PSSetShader(g_edgePS, nullptr, 0);
  ctx->Draw(4, 0);

  // Pass 2: blending-weight calculation (edges at t8) -> weights.
  ctx->PSSetShaderResources(0, 10, nullSRV);
  ctx->ClearRenderTargetView(g_weightRTV, black);
  d3d11SetRenderTargets(ctx, 1, &g_weightRTV, nullptr);
  ID3D11ShaderResourceView* p2[9] = {g_areaSRV, g_searchSRV, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, g_edgesSRV};
  ctx->PSSetShaderResources(0, 9, p2);
  ctx->VSSetShader(g_weightVS, nullptr, 0);
  ctx->PSSetShader(g_weightPS, nullptr, 0);
  ctx->Draw(4, 0);

  // Pass 3: neighborhood blending (colour t2 + weights t9) -> the colour target.
  ctx->PSSetShaderResources(0, 10, nullSRV);
  d3d11SetRenderTargets(ctx, 1, &colorRTV, nullptr);
  ID3D11ShaderResourceView* p3[10] = {g_areaSRV, g_searchSRV, g_sceneSRV,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, g_weightSRV};
  ctx->PSSetShaderResources(0, 10, p3);
  ctx->VSSetShader(g_blendVS, nullptr, 0);
  ctx->PSSetShader(g_blendPS, nullptr, 0);
  ctx->Draw(4, 0);
  ctx->PSSetShaderResources(0, 10, nullSRV);
  return true;
}

}  // namespace

bool smaaPreUiEnabled() {
  static const bool enabled = [] {
    if (!smaaEnabled())
      return false;
    const char* env = std::getenv("DUSK_SMAA_PREUI");
    return !env || env[0] != '0';
  }();
  return enabled;
}

bool smaaEnabled() {
  return featureEnabled(Feature::Smaa);
}

bool smaaApplySceneColor(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene) {
  if (!smaaPreUiEnabled() || !ctx || !scene || g_broken)
    return false;
  if (g_doneThisFrame.load(std::memory_order_relaxed))
    return false;

  D3D11_TEXTURE2D_DESC sd = {};
  scene->GetDesc(&sd);
  // Nothing in this mod multisamples, and these engines render into none of the
  // multisampled targets they allocate, so a multisample surface reaching this
  // pass means the scene test matched something it should not have. Refuse it
  // rather than run a pass whose shaders take a single-sample source.
  if (sd.SampleDesc.Count != 1)
    return false;

  // Claim the frame BEFORE doing anything, not after.
  //
  // This is a re-entry guard, not bookkeeping. Restoring render targets at the
  // end of the pass goes back through the hooked OMSetRenderTargets, which
  // re-enters the boundary check, which sees the scene pair bound again -- and
  // if the latch were still clear at that moment it would start another pass,
  // and another. The first build of this path hung the game on the loading
  // screen for exactly that reason, before the intro video could play.
  if (g_doneThisFrame.exchange(true, std::memory_order_relaxed))
    return false;

  bool expected = false;
  if (!g_passBusy.compare_exchange_strong(expected, true,
                                           std::memory_order_acquire)) {
    g_doneThisFrame.store(false, std::memory_order_relaxed);
    return false;
  }

  ID3D11Device* dev = nullptr;
  scene->GetDevice(&dev);
  if (!dev || !acceptsDevice(dev)) {
    if (dev)
      dev->Release();
    g_passBusy.store(false, std::memory_order_release);
    g_doneThisFrame.store(false, std::memory_order_relaxed);
    return false;
  }

  D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
  rtvDesc.Format = concreteFormat(sd.Format);
  rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  ID3D11RenderTargetView* rtv = nullptr;
  bool ran = false;
  if (SUCCEEDED(dev->CreateRenderTargetView(scene, &rtvDesc, &rtv)) && rtv) {
    // Everything the passes disturb is put back when this leaves scope. The
    // engine is still building this frame.
    ScopedSmaaState saved(ctx);
    ran = smaaRunPasses(dev, ctx, scene, rtv);
    release(rtv);
  }
  if (ran) {
    g_preUiProven.store(true, std::memory_order_relaxed);
    // Reported whenever the SIZE CHANGES, not once.
    //
    // A one-shot here lies by omission the moment two paths can run it at
    // different resolutions. With supersampling on, the boundary pass runs at
    // scene resolution until supersampling engages, and the in-pass call takes
    // over at display resolution afterwards -- a latched line announces the
    // first and can never report the handover, which is exactly how an earlier
    // `FIXES smaa=active size=` line hid which surface SMAA was working on for
    // several sessions.
    static std::atomic<uint32_t> announcedWidth{0};
    static std::atomic<uint32_t> announcedHeight{0};
    D3D11_TEXTURE2D_DESC d = {};
    scene->GetDesc(&d);
    const uint32_t previousWidth =
      announcedWidth.exchange(d.Width, std::memory_order_relaxed);
    const uint32_t previousHeight =
      announcedHeight.exchange(d.Height, std::memory_order_relaxed);
    if (previousWidth != d.Width || previousHeight != d.Height)
      log("SMAA: pre-UI active size=", std::dec, d.Width, "x", d.Height,
          previousWidth ? " (moved from " + std::to_string(previousWidth) +
                          "x" + std::to_string(previousHeight) + ")"
                        : std::string(
                            " (the interface is composited after this and is"
                            " left alone)"));
  }
  if (!ran) {
    // Hand the frame back so the Present path can still antialias it. Claiming
    // the frame is a re-entry guard, and a guard that outlives a failed attempt
    // would turn "the pre-UI pass could not run" into "SMAA does nothing at
    // all, quietly" -- which is the failure mode this project has now made
    // three times. Safe here: the scope guard above has already restored, so
    // nothing can recurse through the reset.
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
      log("SMAA: pre-UI pass could not run on the scene target; falling back to"
          " the Present path, which also antialiases the interface");
  }
  dev->Release();
  g_passBusy.store(false, std::memory_order_release);
  if (!ran)
    g_doneThisFrame.store(false, std::memory_order_relaxed);
  return ran;
}

void smaaFrameReset() {
  g_doneThisFrame.store(false, std::memory_order_relaxed);
}

void smaaApply(IDXGISwapChain* swapChain) {
  if (!smaaEnabled() || !swapChain || g_broken)
    return;
  // The scene-target pass already antialiased this frame, before the interface
  // was composited onto it. Running again here would smooth the UI and text
  // that path exists to leave alone.
  if (g_doneThisFrame.load(std::memory_order_relaxed))
    return;
  // Once the pre-UI path has worked even once, this engine is known to
  // composite its interface separately -- so a frame the pre-UI path did not
  // claim is a frame with no 3D scene in it at all: a logo, the title, a menu.
  // Antialiasing those means softening text and nothing else, which is the
  // whole reason SMAA had to be switched off before. Measured: a run reported
  // the Present path at 1458 ms and the pre-UI path at 10392 ms in the same
  // session, so every menu screen before the first field was being softened.
  if (g_preUiProven.load(std::memory_order_relaxed))
    return;
  // Reaching here means the pre-UI pass did not claim this frame, so SMAA is
  // about to antialias the composited image, interface included. Said once,
  // because "SMAA is on" and "SMAA is on in the good place" are different
  // facts and the shared init line above answers neither.
  {
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true, std::memory_order_relaxed))
      log("SMAA: running at Present over the finished frame -- the interface"
          " is antialiased too. The pre-UI path did not claim this frame.");
  }
  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
    return;
  ID3D11Device* dev = nullptr;
  back->GetDevice(&dev);
  ID3D11DeviceContext* ctx = nullptr;
  if (dev) dev->GetImmediateContext(&ctx);
  ID3D11RenderTargetView* backRTV = nullptr;
  if (dev && ctx)
    dev->CreateRenderTargetView(back, nullptr, &backRTV);
  // No state save/restore here, unlike Arland's pre-UI injection. This runs at
  // Present, after the game has finished submitting the frame, so there is no
  // pending operation of the game's to preserve state for. The pre-UI path,
  // when it exists, will need one.
  if (dev && ctx && backRTV) {
    bool expected = false;
    if (g_passBusy.compare_exchange_strong(expected, true,
                                            std::memory_order_acquire)) {
      if (acceptsDevice(dev))
        smaaRunPasses(dev, ctx, back, backRTV);
      g_passBusy.store(false, std::memory_order_release);
    }
  }
  release(backRTV);
  release(back);
  if (ctx) ctx->Release();
  if (dev) dev->Release();
}

}  // namespace atfix
