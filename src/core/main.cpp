// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
//
// D3D11 proxy entry point. It forwards Direct3D, installs the shared rendering
// hooks for a verified Dusk executable, and gives the engine modules a point at
// which the game image is certainly unpacked.
//
// It knows nothing about either engine. Everything past initializeEngineFixes()
// is dispatched in core/engine.cpp -- see that header for why the two engines
// are split but the DLL is not.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "crash_log.h"
#include "engine.h"
#include "game.h"
#include "hook_util.h"
#include "log.h"
#include "pad_notify_trace.h"
#include "d3d11_hooks.h"
#include "highres.h"
#include "sampler.h"
#include "frame_map.h"
#include "scene_policy.h"
#include "scene_pass.h"
#include "sharpen.h"
#include "smaa.h"
#include "frame_capture.h"
#include "supersample.h"
#include "supersample_policy.h"
#include "util.h"
#include "window_background.h"
#include "version.h"
#include "../../vendor/minhook/include/MinHook.h"

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

// Forwarded untouched. The mod has nothing to do with D3D11-on-12, but a tool
// injected alongside it can import the name statically, and a missing import
// stops the process before it can open a window or write a log.
using PFN_D3D11On12CreateDevice = HRESULT (__stdcall *) (
  IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown**, UINT, UINT,
  ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
  D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

struct D3D11Proc {
  PFN_D3D11CreateDevice             D3D11CreateDevice             = nullptr;
  PFN_D3D11CreateDeviceAndSwapChain D3D11CreateDeviceAndSwapChain = nullptr;
  PFN_D3D11On12CreateDevice         D3D11On12CreateDevice         = nullptr;
};

D3D11Proc loadSystemD3D11() {
  static mutex initMutex;
  static D3D11Proc d3d11Proc;
  // The procedure members are assigned separately. Publishing readiness with
  // the first member lets another caller observe a half-filled table, so use a
  // distinct release/acquire flag and publish only after every lookup.
  static std::atomic<bool> ready{false};

  if (ready.load(std::memory_order_acquire))
    return d3d11Proc;

  std::lock_guard lock(initMutex);

  if (ready.load(std::memory_order_relaxed))
    return d3d11Proc;

  // Before anything else this DLL does, so a fault in our own installation is
  // still caught. Skipped when the mod is stood down: DUSK_DISABLE means a
  // process this DLL has not touched, and an installed exception filter is a
  // thing it has touched.
  if (!std::getenv("DUSK_DISABLE") || std::getenv("DUSK_DISABLE")[0] == '0')
    installCrashLogger();
  log("Atelier Dusk Fixes version ", DUSK_FIX_VERSION);
  log("Title: ", titleName(currentTitle()));
  logConfiguration();

  HMODULE libD3D11 = LoadLibraryExA("d3d11_proxy.dll", nullptr,
    LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

  if (libD3D11) {
    log("D3D11 forwarding: d3d11_proxy.dll");
  } else {
    std::array<char, MAX_PATH + 1> path = { };
    const UINT length = GetSystemDirectoryA(path.data(), MAX_PATH);
    const char suffix[] = "\\d3d11.dll";

    // GetSystemDirectoryA returns the required length when the buffer is too
    // small. The suffix includes its terminator, so this also proves the append
    // stays inside the array.
    if (!length || length + sizeof(suffix) > path.size())
      return D3D11Proc();

    std::memcpy(path.data() + length, suffix, sizeof(suffix));
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
  d3d11Proc.D3D11On12CreateDevice =
    reinterpret_cast<PFN_D3D11On12CreateDevice>(
      GetProcAddress(libD3D11, "D3D11On12CreateDevice"));

  if (!d3d11Proc.D3D11CreateDevice ||
      !d3d11Proc.D3D11CreateDeviceAndSwapChain)
    log("Loaded D3D11 module is missing a device-creation export;"
        " device creation will fail");

  // Failure returns above leave readiness clear so a transient loader failure
  // can retry. A successfully loaded module is immutable after publication.
  ready.store(true, std::memory_order_release);
  return d3d11Proc;
}

using PFN_IDXGISwapChain_Present = HRESULT (STDMETHODCALLTYPE *) (
  IDXGISwapChain*, UINT, UINT);

PFN_IDXGISwapChain_Present originalPresent = nullptr;

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* swapChain,
                                        UINT syncInterval, UINT flags) {
  // The frame boundary. For the Phyre module this is the atlas cache's entire
  // lifetime, not just where the diagnostic attributes out-of-drain locks.
  dusk::engineFrameTick();
  highResFrameTick();
  // Counters and one-shot diagnostics ONLY. Supersampling deliberately does no
  // rendering work at Present: two of its four failed predecessors blacked the
  // screen out with a present-time pass painting over a finished frame, and the
  // absence of one here is this design's safety argument rather than an
  // optimisation. See supersample.h.
  ssaaFrameTick(swapChain);
  frameCaptureTick(swapChain);
  scenePassFrameTick();
  frameMapFrameTick();
  scenePolicy().frameTick();
  sharpenPreload();
  samplerReport();
  // Last thing before the frame is handed over: SMAA runs over the finished
  // image, so everything the game drew this frame has to be in it already.
  smaaApply(swapChain);
  // After smaaApply, never before: this clears the once-per-frame latch that
  // tells the Present path the pre-UI pass already ran. Resetting it earlier
  // would let both run, which is precisely the UI softening the pre-UI path
  // exists to avoid.
  smaaFrameReset();
  return originalPresent(swapChain, syncInterval, flags);
}

// The engine resizes its own swap chain during device init, so clamping only at
// creation is not enough -- ResizeBuffers would put the oversized backbuffer
// straight back and ResizeTarget would ask the display for a mode it does not
// have. Both are clamped for the same reason and by the same rule.
using PFN_ResizeBuffers = HRESULT (STDMETHODCALLTYPE*)(
  IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_ResizeTarget = HRESULT (STDMETHODCALLTYPE*)(
  IDXGISwapChain*, const DXGI_MODE_DESC*);

PFN_ResizeBuffers originalResizeBuffers = nullptr;
PFN_ResizeTarget originalResizeTarget = nullptr;

HRESULT STDMETHODCALLTYPE hookedResizeBuffers(
    IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) {
  ssaaClampPresentSize(&width, &height, "ResizeBuffers");
  return originalResizeBuffers(swapChain, bufferCount, width, height, format,
                               flags);
}

HRESULT STDMETHODCALLTYPE hookedResizeTarget(
    IDXGISwapChain* swapChain, const DXGI_MODE_DESC* mode) {
  if (!mode)
    return originalResizeTarget(swapChain, mode);
  // The caller's structure is never written to; the substitution is made on a
  // copy, as the window-background fix does with its class.
  DXGI_MODE_DESC clamped = *mode;
  ssaaClampPresentSize(&clamped.Width, &clamped.Height, "ResizeTarget");
  return originalResizeTarget(swapChain, &clamped);
}

// Shared failure handling for the two proxy-local hook sets below. Runs the
// rollback and reports what happened, and returns true when the rollback was
// complete, meaning a later swap chain or device may retry. False means this
// attempt may still own a live detour, so the caller must refuse every later
// attempt rather than create a second hook over a target whose state is no
// longer known. Same rule, and the same two log lines, as the central owner in
// d3d11_hooks.cpp.
bool declineHookTransaction(HookTransaction& transaction, const char* what) {
  const HookTransactionFailure& failure = transaction.failure();
  log(what, ": transaction declined stage=",
      hookTransactionStageName(failure.stage), " target=", failure.target,
      failure.status ? " status=" : "",
      failure.status
        ? MH_StatusToString(static_cast<MH_STATUS>(failure.status)) : "");
  if (transaction.rollback()) {
    log(what, ": rolled back completely; a later attempt may retry");
    return true;
  }
  const HookTransactionFailure& rollbackFailure = transaction.rollbackFailure();
  log(what, ": ROLLBACK INCOMPLETE stage=",
      hookTransactionStageName(rollbackFailure.stage),
      " target=", rollbackFailure.target,
      rollbackFailure.status ? " status=" : "",
      rollbackFailure.status
        ? MH_StatusToString(static_cast<MH_STATUS>(rollbackFailure.status))
        : "",
      "; refusing every later attempt because hook ownership is now uncertain");
  return false;
}

// Present lives in the swap chain's vtable, so it can only be hooked once an
// instance exists. Called after each successful device/swapchain creation.
//
// Present and the two resize slots go in as one transaction, because the
// present-size clamp is not a feature without them: the engine resizes its own
// swap chain during device init, so a session that hooked Present and lost
// ResizeBuffers would put the oversized backbuffer straight back while the
// startup log said the clamp was on. Installation is latched only when the
// whole requested set is live, and refused for the rest of the session only
// when a rollback left ownership uncertain.
void hookPresent(IDXGISwapChain* swapChain) {
  static mutex installMutex;
  static bool installed = false;
  static bool poisoned = false;
  if (!swapChain || !dusk::initializeEngineFixes())
    return;
  std::lock_guard lock(installMutex);
  if (installed || poisoned)
    return;
  auto** vtable = *reinterpret_cast<void***>(swapChain);
  // IDXGISwapChain::Present is slot 8 (IUnknown 0-2, IDXGIObject 3-6,
  // IDXGIDeviceSubObject 7, then Present). Slots 13 and 14 are ResizeBuffers
  // and ResizeTarget, counted from the same base: GetBuffer 9,
  // SetFullscreenState 10, GetFullscreenState 11, GetDesc 12, ResizeBuffers 13,
  // ResizeTarget 14. The resize pair is requested only when the clamp is on, so
  // an ordinary session has neither.
  const bool clamps = ssaaPolicy().clampsPresentSize;
  HookTransaction transaction;
  bool created = transaction.create(vtable[8],
    reinterpret_cast<void*>(&hookedPresent),
    reinterpret_cast<void**>(&originalPresent));
  if (created && clamps)
    created =
      transaction.create(vtable[13],
        reinterpret_cast<void*>(&hookedResizeBuffers),
        reinterpret_cast<void**>(&originalResizeBuffers)) &&
      transaction.create(vtable[14],
        reinterpret_cast<void*>(&hookedResizeTarget),
        reinterpret_cast<void**>(&originalResizeTarget));
  if (!created || !transaction.enableAll()) {
    poisoned = !declineHookTransaction(transaction, "Present hook");
    return;
  }
  transaction.commit();
  installed = true;
  log("Present hook installed", clamps
    ? "; SSAA present clamp enabled, ResizeBuffers and ResizeTarget hooked"
    : "");
}

using PFN_IDXGIFactory_CreateSwapChain = HRESULT (STDMETHODCALLTYPE *) (
  IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

PFN_IDXGIFactory_CreateSwapChain originalCreateSwapChain = nullptr;

HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(
    IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
  // Before the call, so the swap chain is created at the display size while the
  // engine's own render size stays where the ini put it. See supersample.h.
  if (desc)
    ssaaClampPresentSize(&desc->BufferDesc.Width, &desc->BufferDesc.Height,
                         "CreateSwapChain");
  const HRESULT result = originalCreateSwapChain(factory, device, desc, swapChain);
  // Reported from the succeeded call, so the line names the swap chain the game
  // actually got rather than the one it asked for.
  if (SUCCEEDED(result) && desc)
    noteSwapChainSize(desc->BufferDesc.Width, desc->BufferDesc.Height,
      desc->BufferDesc.Format, desc->BufferDesc.RefreshRate.Numerator,
      desc->BufferDesc.RefreshRate.Denominator, desc->Windowed != FALSE);
  // After the call: the engine has created its window by now, and the desc
  // carries the clamped size the window should match.
  if (SUCCEEDED(result))
    ssaaFitOutputWindow(desc);
  if (SUCCEEDED(result) && swapChain && *swapChain) {
    // Tag the back buffer. This is supersampling's identity anchor: the bind
    // whose colour target carries this tag is the composite, which is the one
    // fact the whole feature is built on. Both swap-chain routes do it, because
    // Ayesha takes this one and the other games have never been measured.
    ssaaNoteBackBuffer(*swapChain);
    hookPresent(*swapChain);
  }
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
  static mutex installMutex;
  // Installation state, kept separately from originalCreateSwapChain. MinHook
  // publishes the trampoline at create time, so a create that succeeds and an
  // enable that fails would set the pointer with no detour on the target, and
  // using it as the guard would refuse every later device for the session.
  static bool installed = false;
  static bool poisoned = false;
  if (!device || !dusk::initializeEngineFixes())
    return;
  std::lock_guard lock(installMutex);
  if (installed || poisoned)
    return;
  // This is engine-agnostic, so it cannot assume an engine module has already
  // initialized MinHook. On Ayesha the Phyre module does and this used to be
  // invisible; on Escha & Logy and Shallie nothing does, and the hook failed
  // with MH_ERROR_NOT_INITIALIZED on every run. That cost nothing while no fix
  // installs on KTGL, but it silently removes the swap-chain size, which is
  // what the render-target census classifies every target against -- so the
  // census would have reported a wrong `rel=` for all of them. Same idiom as
  // highres.cpp: a second call answers MH_ERROR_ALREADY_INITIALIZED, which is
  // a success here.
  const MH_STATUS init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
    log("Failed to initialize MinHook for the frame boundary: ",
        MH_StatusToString(init));
    return;
  }
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
    HookTransaction transaction;
    if (!transaction.create(vtable[10],
          reinterpret_cast<void*>(&hookedCreateSwapChain),
          reinterpret_cast<void**>(&originalCreateSwapChain)) ||
        !transaction.enableAll()) {
      poisoned = !declineHookTransaction(transaction, "CreateSwapChain hook");
    } else {
      transaction.commit();
      installed = true;
      log("CreateSwapChain hook installed");
    }
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

  // Exact executable recognition is the boundary for every shared Direct3D
  // mutation. A misplaced DLL, or a patched game build whose addresses have
  // not been verified, receives the system call untouched.
  if (!dusk::initializeEngineFixes())
    return (*proc.D3D11CreateDevice)(pAdapter, DriverType, Software, Flags,
      pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
      ppImmediateContext);

  HRESULT hr = (*proc.D3D11CreateDevice)(pAdapter, DriverType, Software, Flags,
    pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
    ppImmediateContext);

  // Ayesha's route: the swap chain arrives later, via the DXGI factory.
  if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
    atfix::hookFactoryForSwapChain(*ppDevice);
    // Installed here, before the game has created anything. A census that
    // starts late silently omits the targets built during startup, which is
    // most of them -- and the fix that shares its hook would miss exactly the
    // same ones, which is worse than missing them in a log.
    atfix::d3d11InstallHooks(*ppDevice,
      ppImmediateContext ? *ppImmediateContext : nullptr);
  }

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

  // Keep the unknown-build path visibly and structurally free of every shared
  // swap-chain transformation and hook. This guard must remain before the
  // descriptor copy and its size clamp below.
  if (!dusk::initializeEngineFixes())
    return (*proc.D3D11CreateDeviceAndSwapChain)(pAdapter, DriverType,
      Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
      pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
      ppImmediateContext);

  // The other swap-chain route, clamped for the same reason as the factory one.
  // The parameter is const, so the substitution is made on a copy and the
  // caller's structure is never written to. Escha & Logy takes the factory
  // route; covering both is what keeps this from depending on which.
  DXGI_SWAP_CHAIN_DESC clampedDesc = {};
  if (pSwapChainDesc) {
    clampedDesc = *pSwapChainDesc;
    atfix::ssaaClampPresentSize(&clampedDesc.BufferDesc.Width,
      &clampedDesc.BufferDesc.Height, "D3D11CreateDeviceAndSwapChain");
    pSwapChainDesc = &clampedDesc;
  }

  HRESULT hr = (*proc.D3D11CreateDeviceAndSwapChain)(pAdapter, DriverType,
    Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc,
    ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

  if (SUCCEEDED(hr) && pSwapChainDesc)
    atfix::noteSwapChainSize(pSwapChainDesc->BufferDesc.Width,
      pSwapChainDesc->BufferDesc.Height, pSwapChainDesc->BufferDesc.Format,
      pSwapChainDesc->BufferDesc.RefreshRate.Numerator,
      pSwapChainDesc->BufferDesc.RefreshRate.Denominator,
      pSwapChainDesc->Windowed != FALSE);

  // Both swap-chain routes, because which one a game takes is the game's
  // choice. The KTGL games observed so far come through the factory hook, but a
  // fix that only covers the route that was tested is a fix with a silent hole.
  if (SUCCEEDED(hr))
    atfix::ssaaFitOutputWindow(pSwapChainDesc);

  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    atfix::ssaaNoteBackBuffer(*ppSwapChain);
    atfix::hookPresent(*ppSwapChain);
  }

  if (SUCCEEDED(hr) && ppDevice && *ppDevice)
    atfix::d3d11InstallHooks(*ppDevice,
      ppImmediateContext ? *ppImmediateContext : nullptr);

  return hr;
}

// Pass-through only. This export exists for compatibility with tools that
// import it; no Dusk hook or rendering feature is installed through this API.
DLLEXPORT HRESULT __stdcall D3D11On12CreateDevice(
        IUnknown*             pDevice,
        UINT                  Flags,
  const D3D_FEATURE_LEVEL*    pFeatureLevels,
        UINT                  FeatureLevels,
        IUnknown**            ppCommandQueues,
        UINT                  NumQueues,
        UINT                  NodeMask,
        ID3D11Device**        ppDevice,
        ID3D11DeviceContext** ppImmediateContext,
        D3D_FEATURE_LEVEL*    pChosenFeatureLevel) {
  atfix::D3D11Proc proc = atfix::loadSystemD3D11();

  if (!proc.D3D11On12CreateDevice)
    return E_NOTIMPL;

  return proc.D3D11On12CreateDevice(pDevice, Flags, pFeatureLevels,
    FeatureLevels, ppCommandQueues, NumQueues, NodeMask, ppDevice,
    ppImmediateContext, pChosenFeatureLevel);
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH: {
      DisableThreadLibraryCalls(hinstDLL);
      // The window-background fix hooks a user32 API and has to be in place
      // before the game registers its window class, which happens well before
      // it reaches D3D11. That is why MinHook comes up here rather than at
      // device creation; every later MH_Initialize call in this DLL accepts
      // ALREADY_INITIALIZED. Skipped in the documented pass-through mode.
      const char* disable = std::getenv("DUSK_DISABLE");
      if (!disable || disable[0] == '0') {
        MH_Initialize();
        atfix::installWindowBackgroundFix();
        // Same reason as the line above: the window is created before the game
        // reaches D3D11, so a hook installed when the proxy is first used is
        // already too late. Which engine's, and whether any, is engine.cpp's
        // question -- this file does not know the modules exist.
        dusk::installEngineEarlyFixes();
      }
      break;
    }
    case DLL_PROCESS_DETACH:
      // Only on dynamic unload. Any asynchronous callback or worker pins this
      // DLL before it is published, so dynamic detach cannot race either one.
      // In the unpinned early-load case only MinHook can have been initialized.
      // Process exit (lpvReserved non-null) must not enter MinHook after other
      // threads may have been terminated while holding its internal spin lock.
      if (!lpvReserved)
        MH_Uninitialize();
      break;
    default:
      break;
  }
  return TRUE;
}
