// SPDX-License-Identifier: MIT
//
// Implementation. Why this is a pass rather than a narrowed viewport is in
// letterbox_present.h; what is here is the pass and the notes that only mean
// anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>

#include "letterbox_present.h"
#include "../../core/d3d11_hooks.h"
#include "../../core/letterbox.h"
#include "../../core/log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
  const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
  ID3DBlob**, ID3DBlob**);

HMODULE g_compiler = nullptr;

// Re-entry guard, thread-local for the same reason sharpen.cpp's is: a second
// thread arriving here has every right to be served. This pass binds and draws
// through the unhooked originals, so it should never re-enter at all; the guard
// is what makes that a checked property rather than an assumption.
thread_local bool t_inPass = false;

bool g_ready = false;
bool g_broken = false;
ID3D11Device* g_ownerDevice = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11RasterizerState* g_raster = nullptr;

// The finished frame is copied here and sampled from here, so the pass reads
// one resource and writes another. Kept for the session, rebuilt when the back
// buffer changes size or format.
ID3D11Texture2D* g_scratch = nullptr;
ID3D11ShaderResourceView* g_scratchSRV = nullptr;
UINT g_scratchWidth = 0, g_scratchHeight = 0;
DXGI_FORMAT g_scratchFormat = DXGI_FORMAT_UNKNOWN;

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
    log("LETTERBOX: a second D3D11 device reached the pass; refusing it so"
        " resources from the first device are never bound across devices");
  return false;
}

// A full-screen triangle and a plain sample. The scaling is done entirely by
// the viewport, so the shader does not need to know the frame is being fitted.
const char* kHlsl = R"HLSL(
Texture2D    src  : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.uv = t;
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 PSMain(VSOut i) : SV_TARGET {
  return float4(src.SampleLevel(samp, i.uv, 0).rgb, 1.0);
}
)HLSL";

bool compile(PFN_D3DCompile fn, const char* entry, const char* target,
             ID3DBlob** out) {
  ID3DBlob* err = nullptr;
  const HRESULT hr = fn(kHlsl, std::strlen(kHlsl), "dusk_letterbox", nullptr,
                        nullptr, entry, target, 0, 0, out, &err);
  if (FAILED(hr) || !*out)
    log("LETTERBOX: ", entry, " failed to compile: ",
        err ? static_cast<const char*>(err->GetBufferPointer()) : "(no message)");
  if (err) err->Release();
  return SUCCEEDED(hr) && *out;
}

bool init(ID3D11Device* device) {
  if (g_ready) return true;

  // The compiler is loaded from the frame tick, never from here. LoadLibrary
  // takes the loader lock, and sharpen.cpp records what that cost: with edge
  // smoothing off it was the first caller and Escha hung on the loading screen.
  // Tested before the one-attempt latch, because "not preloaded yet" is true at
  // most once and latching on it would turn a one-frame wait into the fix being
  // off for the whole process.
  HMODULE compiler = g_compiler;
  if (!compiler) {
    static std::atomic<bool> waited{false};
    if (!waited.exchange(true, std::memory_order_relaxed))
      log("LETTERBOX: d3dcompiler is not loaded yet; the frame is fitted from a"
          " later frame, so the first moments of a session are unfitted");
    return false;
  }
  if (g_broken) return false;
  g_broken = true;   // one attempt
  auto D3DCompile = reinterpret_cast<PFN_D3DCompile>(
    GetProcAddress(compiler, "D3DCompile"));
  if (!D3DCompile) { log("LETTERBOX: no D3DCompile"); return false; }

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

  // Linear, because this pass only ever shrinks the frame -- point sampling a
  // 1200-line image into 900 lines drops every fourth row outright.
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sd, &g_sampler))) return false;

  // Every channel, unlike the sharpening pass. That one runs mid-frame on a
  // target whose alpha the engine still consumes; this owns the back buffer
  // outright and nothing reads it again before Present.
  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->CreateBlendState(&bd, &g_blend))) return false;

  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = FALSE;
  dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  if (FAILED(device->CreateDepthStencilState(&dd, &g_depth))) return false;

  // Scissor disabled on purpose. The viewport already confines the draw, and a
  // scissor left over from the engine's last pass would clip this one.
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  rd.ScissorEnable = FALSE;
  if (FAILED(device->CreateRasterizerState(&rd, &g_raster))) return false;

  g_broken = false;
  g_ready = true;
  return true;
}

DXGI_FORMAT concrete(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
  }
}

bool ensureScratch(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& sd) {
  const DXGI_FORMAT view = concrete(sd.Format);
  if (g_scratch && g_scratchWidth == sd.Width && g_scratchHeight == sd.Height &&
      g_scratchFormat == view)
    return true;
  release(g_scratchSRV);
  release(g_scratch);
  g_scratchWidth = g_scratchHeight = 0;
  g_scratchFormat = DXGI_FORMAT_UNKNOWN;

  D3D11_TEXTURE2D_DESC td = sd;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.SampleDesc.Quality = 0;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = 0;
  td.MiscFlags = 0;
  D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
  vd.Format = view;
  vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  vd.Texture2D.MipLevels = 1;
  // Through the ORIGINAL device: the mod's own surfaces must not travel through
  // the mod's own hooks.
  if (FAILED(createTexture2DUnhooked(device, &td, nullptr, &g_scratch)) ||
      !g_scratch ||
      FAILED(device->CreateShaderResourceView(g_scratch, &vd, &g_scratchSRV))) {
    release(g_scratchSRV);
    release(g_scratch);
    return false;
  }
  g_scratchWidth = sd.Width;
  g_scratchHeight = sd.Height;
  g_scratchFormat = view;
  return true;
}

void preload() {
  if (g_compiler)
    return;
  g_compiler = LoadLibraryA("d3dcompiler_47.dll");
  if (!g_compiler)
    g_compiler = LoadLibraryA("d3dcompiler.dll");
  if (!g_compiler) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
      log("LETTERBOX: no d3dcompiler; the frame cannot be fitted this session");
  }
}

bool apply(IDXGISwapChain* swapChain) {
  if (!swapChain || t_inPass)
    return false;

  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
    return false;
  D3D11_TEXTURE2D_DESC bd = {};
  back->GetDesc(&bd);

  // The rectangle, and the whole gate. letterboxViewportFor declines for a
  // 16:9 back buffer, for a game whose matrix row is Unsupported, and for any
  // surface that is not the one the swap chain was noted at.
  D3D11_VIEWPORT fitted = {};
  if (bd.SampleDesc.Count != 1 ||
      !letterboxViewportFor(bd.Width, bd.Height, &fitted)) {
    release(back);
    return false;
  }

  ID3D11Device* device = nullptr;
  back->GetDevice(&device);
  ID3D11DeviceContext* ctx = nullptr;
  if (device)
    device->GetImmediateContext(&ctx);

  bool ran = false;
  if (device && ctx) {
    t_inPass = true;
    if (acceptsDevice(device) && init(device) && ensureScratch(device, bd)) {
      D3D11_RENDER_TARGET_VIEW_DESC rd = {};
      rd.Format = concrete(bd.Format);
      rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      ID3D11RenderTargetView* rtv = nullptr;
      if (SUCCEEDED(device->CreateRenderTargetView(back, &rd, &rtv)) && rtv) {
        // EVERY BIND AND THE DRAW GO THROUGH THE UNHOOKED ORIGINALS. This is
        // the detail the whole pass turns on. Supersampling corrects the
        // viewport of any draw whose colour target is the back buffer, and it
        // corrects it to the FULL SURFACE -- so a draw made through the public
        // methods would have its fitted viewport replaced a moment before it
        // landed, and the frame would come out stretched with no sign of why.
        // That is precisely what happened to SMAA's three passes.
        const ContextOriginals& originals = d3d11OriginalsFor(ctx);

        if (originals.copyResource)
          originals.copyResource(ctx, g_scratch, back);
        else
          ctx->CopyResource(g_scratch, back);

        // The bars. Every frame, because nothing else writes those pixels and
        // last frame's edge would otherwise stay in them.
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearRenderTargetView(rtv, black);

        if (originals.omSetRenderTargets)
          originals.omSetRenderTargets(ctx, 1, &rtv, nullptr);
        else
          d3d11SetRenderTargets(ctx, 1, &rtv, nullptr);
        if (originals.rsSetViewports)
          originals.rsSetViewports(ctx, 1, &fitted);
        else
          ctx->RSSetViewports(1, &fitted);

        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_vs, nullptr, 0);
        ctx->PSSetShader(g_ps, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &g_sampler);
        ctx->PSSetShaderResources(0, 1, &g_scratchSRV);
        ctx->OMSetBlendState(g_blend, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(g_depth, 0);
        ctx->RSSetState(g_raster);

        if (originals.draw)
          originals.draw(ctx, 3, 0);
        else
          ctx->Draw(3, 0);

        // The scratch is bound as a shader resource and is about to be a copy
        // destination again next frame. Leaving it bound makes that copy a
        // hazard the runtime has to resolve by unbinding it anyway, with a
        // warning on a debug device.
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        ran = true;

        // NO STATE RESTORE, and it is the same argument smaa.cpp makes for its
        // own Present pass: the game has finished submitting this frame, so
        // there is no pending operation of its own to preserve state for. This
        // runs later still -- after that pass -- so there is even less.
        static std::atomic<bool> announced{false};
        if (!announced.exchange(true, std::memory_order_relaxed))
          log("FIXES letterbox=fitted at Present, ", std::dec,
              unsigned(fitted.Width), "x", unsigned(fitted.Height), " at ",
              int(fitted.TopLeftX), ",", int(fitted.TopLeftY), " inside ",
              bd.Width, "x", bd.Height);
      }
      release(rtv);
    }
    t_inPass = false;
  }

  if (ctx) ctx->Release();
  release(device);
  release(back);
  return ran;
}

}  // namespace

void installKtglLetterbox() {
  letterboxSetFitPass(&apply, &preload);
  log("KTGL letterbox: registered -- the frame is fitted once at Present");
}

}  // namespace atfix
