// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Sole owner of the D3D11 device and context vtables.
//
// WHY THIS EXISTS. MinHook hooks a function ADDRESS, and these addresses come
// from vtables shared by every object of a class. Two modules hooking the same
// vtable independently is how an enable/disable race gets written by accident,
// and how a partially-installed set survives a failure that should have rolled
// the whole thing back. So exactly one module does it, and the features say
// what they want rather than reaching for MinHook themselves.
//
// This used to live inside highres.cpp, on the reasoning that the file already
// owned the device vtable for its own CreateTexture2D hook. That held while
// there was one feature. It stopped holding once the scene-pass detours and
// supersampling needed slots too: the resolution fix ended up hosting detours
// belonging to features it has nothing to do with, and "which module owns the
// vtables" became invisible.
//
// The split now is: this file owns installation and the trampolines; each
// feature owns its own detours and its own policy. Nothing here knows what a
// render target is for.
//
// VTABLE SLOTS. Every number in this module is enumerated from the MinGW
// d3d11.h this cross-build actually uses, not counted by hand -- a slot off by
// three has already cost this project a confidently wrong result in a different
// context. Regenerate them with:
//
//   podman exec atfix-build bash -lc 'awk "/ID3D11DeviceVtbl/,/^} ID3D11DeviceVtbl/"
//     /usr/x86_64-w64-mingw32/sys-root/mingw/include/d3d11.h
//     | grep -o "STDMETHODCALLTYPE .[A-Za-z_]*" | sed "s/.*[*]//" | nl -v0'
//
// (joined onto one line; the continuations are split here only for width)
//
// and the same with ID3D11DeviceContextVtbl. The context numbers are
// corroborated by working code: d3d11_probe.cpp hooks Map at 14 successfully.
namespace atfix {

// ---- device ---------------------------------------------------------------

using PFN_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *) (
  ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
  ID3D11Texture2D**);
using PFN_CreateSamplerState = HRESULT (STDMETHODCALLTYPE *) (
  ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
// The trampolines for whichever device methods were hooked. A member is null
// when that hook was not requested or could not be installed, so a detour must
// never be reachable without its own original being set -- which it cannot be,
// since an uninstalled hook is never called.
struct DeviceOriginals {
  PFN_CreateTexture2D createTexture2D = nullptr;
  PFN_CreateSamplerState createSamplerState = nullptr;
};

const DeviceOriginals& d3d11DeviceOriginals();

// Create a texture the way the mod's own passes must: never through the hooked
// device, so the high-resolution fix cannot rewrite a target the mod owns.
//
// The original is only captured when that fix installs its device hook, and the
// fix is unsupported on Escha & Logy and Shallie -- so on those two the pointer
// is null and calling it is a jump to zero. When nothing is hooked there is
// nothing to bypass, and the device's own method is the unhooked path already.
inline HRESULT createTexture2DUnhooked(ID3D11Device* device,
                                       const D3D11_TEXTURE2D_DESC* desc,
                                       const D3D11_SUBRESOURCE_DATA* data,
                                       ID3D11Texture2D** out) {
  const DeviceOriginals& originals = d3d11DeviceOriginals();
  return originals.createTexture2D
    ? originals.createTexture2D(device, desc, data, out)
    : device->CreateTexture2D(desc, data, out);
}

// ---- context --------------------------------------------------------------

using PFN_RSSetViewports = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN_RSSetScissorRects = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using PFN_Draw = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexed = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_DrawInstanced = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_OMSetRenderTargets = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
  ID3D11DepthStencilView*);
using PFN_PSSetShaderResources = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PFN_FinishCommandList = HRESULT (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PFN_OMSetRenderTargetsAndUnorderedAccessViews =
  void (STDMETHODCALLTYPE *) (
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
    ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*,
    const UINT*);

// The originals, one set per context vtable.
//
// This is the part two earlier attempts got wrong, and it cost two runs to
// find. Hooking the immediate context's vtable hooks only contexts of that
// class, and D3D11 gives deferred contexts a different class with a different
// vtable. Ayesha issues its draws and its raster state on a DEFERRED context:
// an instrumented 1080p run recorded rsViewports=0 and draws=0 across 1500
// frames with every hook installed and reporting success, while the
// device-level CreateTexture2D hook on the same run fired normally.
//
// (That also explains why d3d11_probe.cpp's Map hook has always worked on the
// immediate context: texture uploads go through it. Drawing does not.)
struct ContextOriginals {
  PFN_RSSetViewports rsSetViewports = nullptr;
  PFN_RSSetScissorRects rsSetScissorRects = nullptr;
  PFN_Draw draw = nullptr;
  PFN_DrawIndexed drawIndexed = nullptr;
  PFN_DrawInstanced drawInstanced = nullptr;
  PFN_DrawIndexedInstanced drawIndexedInstanced = nullptr;
  PFN_OMSetRenderTargets omSetRenderTargets = nullptr;
  PFN_PSSetShaderResources psSetShaderResources = nullptr;
  PFN_FinishCommandList finishCommandList = nullptr;
  PFN_OMSetRenderTargetsAndUnorderedAccessViews
    omSetRenderTargetsAndUnorderedAccessViews = nullptr;
};

// Which set a detour must forward to, decided from the context's own vtable
// pointer rather than from object identity: the engine may hold several
// deferred contexts and they all share one vtable.
const ContextOriginals& d3d11OriginalsFor(ID3D11DeviceContext* context);

// Set render targets without re-entering our own OMSetRenderTargets detour,
// falling back to the public method when that hook is not installed.
//
// Any module doing its own internal binds should use this. A present-time pass
// that binds through the public method walks back into the scene-pass boundary
// logic for binds it was never meant to examine -- harmless today only because
// the immediate context happens to hold no scene marker by then, which is a
// property of one engine and not a guarantee.
void d3d11SetRenderTargets(ID3D11DeviceContext* context, UINT numViews,
                           ID3D11RenderTargetView* const* views,
                           ID3D11DepthStencilView* depth);

// ---- installation ---------------------------------------------------------

// Install every hook the enabled features ask for. Idempotent, and safe to call
// from every device-creation path -- which it is, because the point at which
// the game reaches D3D11 is the earliest we can be sure its image is unpacked.
//
// Installs nothing when no feature wants anything, so an ordinary session with
// every optional feature off has no hooks here at all.
void d3d11InstallHooks(ID3D11Device* device, ID3D11DeviceContext* context);

}  // namespace atfix
