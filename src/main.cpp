// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
//
// D3D11 proxy entry point. Deliberately thin compared to the Arland project's
// main.cpp: this repository ships no rendering features yet, so the proxy exists
// to forward Direct3D, to hook Present as a frame boundary, and to give the
// diagnostic in atlas_stats.cpp a place to install itself.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <array>
#include <cstring>

#include "atlas_fix.h"
#include "game.h"
#include "log.h"
#include "util.h"
#include "version.h"
#include "../vendor/minhook/include/MinHook.h"

#ifdef _MSC_VER
  #define DLLEXPORT
#else
  #define DLLEXPORT __declspec(dllexport)
#endif

namespace atfix {

Log log("dusk-fix.log");

using PFN_D3D11CreateDevice = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
  D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

struct D3D11Proc {
  PFN_D3D11CreateDevice             D3D11CreateDevice             = nullptr;
  PFN_D3D11CreateDeviceAndSwapChain D3D11CreateDeviceAndSwapChain = nullptr;
};

D3D11Proc loadSystemD3D11() {
  static mutex initMutex;
  static D3D11Proc d3d11Proc;

  if (d3d11Proc.D3D11CreateDevice)
    return d3d11Proc;

  std::lock_guard lock(initMutex);

  if (d3d11Proc.D3D11CreateDevice)
    return d3d11Proc;

  log("Atelier Dusk Fixes version ", DUSK_FIX_VERSION);
  log("Title: ", titleName(currentTitle()));

  HMODULE libD3D11 = LoadLibraryExA("d3d11_proxy.dll", nullptr,
    LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

  if (libD3D11) {
    log("D3D11 forwarding: d3d11_proxy.dll");
  } else {
    std::array<char, MAX_PATH + 1> path = { };

    if (!GetSystemDirectoryA(path.data(), MAX_PATH))
      return D3D11Proc();

    std::strncat(path.data(), "\\d3d11.dll", MAX_PATH);
    log("D3D11 forwarding: system d3d11.dll");
    libD3D11 = LoadLibraryA(path.data());

    if (!libD3D11) {
      log("Failed to load d3d11.dll (", path.data(), ")");
      return D3D11Proc();
    }
  }

  d3d11Proc.D3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(
    GetProcAddress(libD3D11, "D3D11CreateDevice"));
  d3d11Proc.D3D11CreateDeviceAndSwapChain =
    reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
      GetProcAddress(libD3D11, "D3D11CreateDeviceAndSwapChain"));

  dusk::initializeAtlasFix();
  return d3d11Proc;
}

using PFN_IDXGISwapChain_Present = HRESULT (STDMETHODCALLTYPE *) (
  IDXGISwapChain*, UINT, UINT);

PFN_IDXGISwapChain_Present originalPresent = nullptr;

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* swapChain,
                                        UINT syncInterval, UINT flags) {
  // The frame boundary the diagnostic attributes out-of-drain locks to.
  dusk::atlasFixFrameTick();
  return originalPresent(swapChain, syncInterval, flags);
}

// Present lives in the swap chain's vtable, so it can only be hooked once an
// instance exists. Called after each successful device/swapchain creation.
void hookPresent(IDXGISwapChain* swapChain) {
  static bool done = false;
  if (done || !swapChain || !dusk::initializeAtlasFix())
    return;
  auto** vtable = *reinterpret_cast<void***>(swapChain);
  // IDXGISwapChain::Present is slot 8 (IUnknown 0-2, IDXGIObject 3-6,
  // IDXGIDeviceSubObject 7, then Present).
  void* target = vtable[8];
  if (MH_CreateHook(target, reinterpret_cast<void*>(&hookedPresent),
                    reinterpret_cast<void**>(&originalPresent)) != MH_OK)
    return;
  if (MH_EnableHook(target) != MH_OK)
    return;
  done = true;
  log("Present hook installed");
}

using PFN_IDXGIFactory_CreateSwapChain = HRESULT (STDMETHODCALLTYPE *) (
  IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

PFN_IDXGIFactory_CreateSwapChain originalCreateSwapChain = nullptr;

HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(
    IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
  const HRESULT result = originalCreateSwapChain(factory, device, desc, swapChain);
  if (SUCCEEDED(result) && swapChain && *swapChain)
    hookPresent(*swapChain);
  return result;
}

// Ayesha reaches its swap chain through D3D11CreateDevice followed by
// IDXGIFactory::CreateSwapChain, not D3D11CreateDeviceAndSwapChain, so hooking
// only the latter leaves Present unhooked and the frame boundary never fires.
// The first diagnostic run hit exactly that: out-of-drain counters accumulated
// and were never flushed, which reads as "no out-of-drain locks" when it
// actually means "never looked". The Arland proxy hooks both routes for the same
// reason.
void hookFactoryForSwapChain(ID3D11Device* device) {
  if (!device || originalCreateSwapChain || !dusk::initializeAtlasFix())
    return;
  IDXGIDevice* dxgiDevice = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory* factory = nullptr;
  HRESULT result = device->QueryInterface(
    IID_IDXGIDevice, reinterpret_cast<void**>(&dxgiDevice));
  if (SUCCEEDED(result))
    result = dxgiDevice->GetAdapter(&adapter);
  if (SUCCEEDED(result))
    result = adapter->GetParent(
      IID_IDXGIFactory, reinterpret_cast<void**>(&factory));
  if (FAILED(result) || !factory) {
    log("Failed to obtain DXGI factory for the frame boundary");
  } else {
    // IDXGIFactory::CreateSwapChain is slot 10.
    void** vtable = *reinterpret_cast<void***>(factory);
    MH_STATUS status = MH_CreateHook(vtable[10],
      reinterpret_cast<void*>(&hookedCreateSwapChain),
      reinterpret_cast<void**>(&originalCreateSwapChain));
    if (!status || status == MH_ERROR_ALREADY_CREATED)
      status = MH_EnableHook(vtable[10]);
    if (status)
      log("Failed to hook IDXGIFactory::CreateSwapChain: ",
        MH_StatusToString(status));
    else
      log("CreateSwapChain hook installed");
  }
  if (factory)
    factory->Release();
  if (adapter)
    adapter->Release();
  if (dxgiDevice)
    dxgiDevice->Release();
}

}  // namespace atfix

extern "C" {

DLLEXPORT HRESULT __stdcall D3D11CreateDevice(
        IDXGIAdapter*            pAdapter,
        D3D_DRIVER_TYPE          DriverType,
        HMODULE                  Software,
        UINT                     Flags,
  const D3D_FEATURE_LEVEL*       pFeatureLevels,
        UINT                     FeatureLevels,
        UINT                     SDKVersion,
        ID3D11Device**           ppDevice,
        D3D_FEATURE_LEVEL*       pFeatureLevel,
        ID3D11DeviceContext**    ppImmediateContext) {
  atfix::D3D11Proc proc = atfix::loadSystemD3D11();

  if (!proc.D3D11CreateDevice)
    return E_FAIL;

  HRESULT hr = (*proc.D3D11CreateDevice)(pAdapter, DriverType, Software, Flags,
    pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
    ppImmediateContext);

  // Ayesha's route: the swap chain arrives later, via the DXGI factory.
  if (SUCCEEDED(hr) && ppDevice && *ppDevice)
    atfix::hookFactoryForSwapChain(*ppDevice);

  return hr;
}

DLLEXPORT HRESULT __stdcall D3D11CreateDeviceAndSwapChain(
        IDXGIAdapter*            pAdapter,
        D3D_DRIVER_TYPE          DriverType,
        HMODULE                  Software,
        UINT                     Flags,
  const D3D_FEATURE_LEVEL*       pFeatureLevels,
        UINT                     FeatureLevels,
        UINT                     SDKVersion,
  const DXGI_SWAP_CHAIN_DESC*    pSwapChainDesc,
        IDXGISwapChain**         ppSwapChain,
        ID3D11Device**           ppDevice,
        D3D_FEATURE_LEVEL*       pFeatureLevel,
        ID3D11DeviceContext**    ppImmediateContext) {
  atfix::D3D11Proc proc = atfix::loadSystemD3D11();

  if (!proc.D3D11CreateDeviceAndSwapChain)
    return E_FAIL;

  HRESULT hr = (*proc.D3D11CreateDeviceAndSwapChain)(pAdapter, DriverType,
    Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc,
    ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    atfix::hookPresent(*ppSwapChain);

  return hr;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(hinstDLL);
      break;
    case DLL_PROCESS_DETACH:
      // Only on real process teardown (lpvReserved non-null means the process is
      // exiting and the loader will not run other DLLs' cleanup reliably).
      if (!lpvReserved)
        MH_Uninitialize();
      break;
    default:
      break;
  }
  return TRUE;
}
