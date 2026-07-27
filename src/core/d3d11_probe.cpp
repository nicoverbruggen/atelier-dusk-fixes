// SPDX-License-Identifier: MIT
//
// See d3d11_probe.h for what this answers and why.
//
// Vtable slot numbers, verified against Wine's d3d11.h (include/wine/windows/
// d3d11.h in the wine-staging tree used locally), which mirrors the Microsoft
// header the game and MinGW both build against:
//
//   IUnknown            : QueryInterface, AddRef, Release           -- 0-2
//   ID3D11DeviceChild    : GetDevice, Get/SetPrivateData,
//                          SetPrivateDataInterface                  -- 3-6
//   ID3D11DeviceContext  : its own methods in declaration order,
//                          starting at slot 7
//
// Counting ID3D11DeviceContext's own methods from VSSetConstantBuffers (7):
// Map is the 8th (7+7=14). CopySubresourceRegion, CopyResource and
// UpdateSubresource are three declared back to back, the 40th/41st/42nd
// (7+39=46, +47, +48). All four match the numbers this was scoped against.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "d3d11_probe.h"
#include "game.h"
#include "log.h"
#include "util.h"
#include "../../vendor/minhook/include/MinHook.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// Every 300 Present calls, matching the cadence the other diagnostics use for
// their periodic reports (ATLASVERIFY's kVerifyReportInterval).
constexpr uint64_t kReportInterval = 300;

enum EntryPoint : uint8_t {
  kEntryMap,
  kEntryUpdateSubresource,
  kEntryCopyResource,
  kEntryCopySubresourceRegion,
};

const char* entryName(EntryPoint entry) {
  switch (entry) {
    case kEntryMap:                   return "Map";
    case kEntryUpdateSubresource:     return "UpdateSubresource";
    case kEntryCopyResource:          return "CopyResource";
    case kEntryCopySubresourceRegion: return "CopySubresourceRegion";
  }
  return "?";
}

// Only the write map types are candidates (per the task: READ and READ_WRITE
// do not count as a write path). Returns nullptr for anything else, which
// callers use as the "not a write" signal.
const char* writeMapTypeName(D3D11_MAP mapType) {
  switch (mapType) {
    case D3D11_MAP_WRITE:              return "WRITE";
    case D3D11_MAP_WRITE_DISCARD:      return "WRITE_DISCARD";
    case D3D11_MAP_WRITE_NO_OVERWRITE: return "WRITE_NO_OVERWRITE";
    default:                           return nullptr;
  }
}

// Cached like atlas_fix.cpp's censusModuleBase: the module base cannot change
// for the process lifetime, and GetModuleHandleW is not free enough to call on
// every hooked D3D11 call.
uintptr_t probeModuleBase() {
  static const uintptr_t base =
    reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  return base;
}

// The game's own call site for whichever hook is running.
//
// This must be evaluated inside a hook body and never one call deeper:
// duskReturnAddress() is frame-based, so it yields the game's return address
// only in the function MinHook actually redirects. Being a macro rather than a
// function is what makes that work -- and what makes it safe to defer, since
// the frame is valid for the whole call.
//
// Deferred deliberately. Map is one of the hottest functions in a D3D11 title
// and the overwhelming majority of its calls are dynamic BUFFER maps, which
// as512Texture rejects on the GetType check before any QueryInterface. Reading
// the caller up front would charge every one of those calls for a result only
// the 512x512 case ever uses.
#define callerRva() \
  uintptr_t(reinterpret_cast<uintptr_t>(duskReturnAddress()) - \
            probeModuleBase())

// True if `resource` is a 2D texture with both dimensions 512, i.e. one of the
// three Ayesha font atlases by shape. `descOut` is filled whenever this
// returns true, so callers can log the fields the report wants without a
// second QueryInterface.
bool as512Texture(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& descOut) {
  if (!resource)
    return false;
  D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  resource->GetType(&dimension);
  if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    return false;
  ID3D11Texture2D* texture2D = nullptr;
  if (FAILED(resource->QueryInterface(IID_ID3D11Texture2D,
                                       reinterpret_cast<void**>(&texture2D))) ||
      !texture2D)
    return false;
  texture2D->GetDesc(&descOut);
  texture2D->Release();
  return descOut.Width == 512 && descOut.Height == 512;
}

// Identifies one (entry point, call site, resource) tuple, which is the
// granularity the task asks for: log once per distinct tuple, not per call --
// the game can issue thousands of calls a frame through the same site.
struct SiteKey {
  uint8_t   entry;
  uintptr_t callerRva;
  uintptr_t resource;

  bool operator == (const SiteKey& o) const {
    return entry == o.entry && callerRva == o.callerRva &&
           resource == o.resource;
  }
};

struct SiteKeyHash {
  size_t operator () (const SiteKey& k) const {
    size_t h = std::hash<uintptr_t>()(k.callerRva);
    h ^= std::hash<uintptr_t>()(k.resource) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
    h ^= size_t(k.entry) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};

constexpr size_t kMaxSites = 4096;
bool g_sitesOverflowed = false;

atfix::mutex g_sitesMutex;
// Guarded by g_sitesMutex. Value is the running per-site count referenced in
// the log line the first time a tuple is seen.
std::unordered_map<SiteKey, uint64_t, SiteKeyHash> g_sites;
// Total matching calls across every site, checked by the periodic summary:
// zero for the whole session is the finding this probe exists to produce.
std::atomic<uint64_t> g_totalWrites{0};

bool g_installed = false;

// Logs once per distinct (entry, callerRva, resource) tuple; every later call
// through the same tuple only advances its count silently, which is what
// keeps a hot call site from flooding the log.
void reportIfNew(EntryPoint entry, ID3D11Resource* resource,
                 uintptr_t callerRva, const D3D11_TEXTURE2D_DESC& desc,
                 const char* mapTypeLabel) {
  const SiteKey key{ uint8_t(entry), callerRva,
                     reinterpret_cast<uintptr_t>(resource) };
  uint64_t count = 0;
  bool firstSeen = false;
  {
    std::lock_guard lock(g_sitesMutex);
    if (g_sites.size() >= kMaxSites && !g_sites.count(key)) {
      // Stop growing rather than stop counting: the total below is what the
      // finding rests on, and it stays exact.
      g_sitesOverflowed = true;
      g_totalWrites.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    uint64_t& c = g_sites[key];
    firstSeen = c == 0;
    count = ++c;
  }
  g_totalWrites.fetch_add(1, std::memory_order_relaxed);
  if (!firstSeen)
    return;
  log("D3D11PROBE WRITE entry=", entryName(entry),
      " resource=", reinterpret_cast<void*>(resource),
      " format=", uint32_t(desc.Format),
      " usage=", uint32_t(desc.Usage),
      " bindFlags=0x", std::hex, desc.BindFlags,
      " cpuAccessFlags=0x", desc.CPUAccessFlags, std::dec,
      mapTypeLabel ? " mapType=" : "", mapTypeLabel ? mapTypeLabel : "",
      " callerRva=", reinterpret_cast<void*>(callerRva),
      " count=", count);
}

using PFN_Map = HRESULT (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
  D3D11_MAPPED_SUBRESOURCE*);
using PFN_UpdateSubresource = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
  UINT, UINT);
using PFN_CopyResource = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using PFN_CopySubresourceRegion = void (STDMETHODCALLTYPE *) (
  ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
  ID3D11Resource*, UINT, const D3D11_BOX*);

PFN_Map originalMap = nullptr;
PFN_UpdateSubresource originalUpdateSubresource = nullptr;
PFN_CopyResource originalCopyResource = nullptr;
PFN_CopySubresourceRegion originalCopySubresourceRegion = nullptr;

HRESULT STDMETHODCALLTYPE hookedMap(
    ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource,
    D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped) {
  const HRESULT hr =
    originalMap(self, resource, subresource, mapType, mapFlags, mapped);
  const char* label = writeMapTypeName(mapType);
  if (SUCCEEDED(hr) && label) {
    D3D11_TEXTURE2D_DESC desc;
    if (as512Texture(resource, desc))
      reportIfNew(kEntryMap, resource, callerRva(), desc, label);
  }
  return hr;
}

void STDMETHODCALLTYPE hookedUpdateSubresource(
    ID3D11DeviceContext* self, ID3D11Resource* dstResource,
    UINT dstSubresource, const D3D11_BOX* dstBox, const void* srcData,
    UINT srcRowPitch, UINT srcDepthPitch) {
  D3D11_TEXTURE2D_DESC desc;
  if (as512Texture(dstResource, desc))
    reportIfNew(kEntryUpdateSubresource, dstResource, callerRva(), desc,
                nullptr);
  originalUpdateSubresource(self, dstResource, dstSubresource, dstBox,
                            srcData, srcRowPitch, srcDepthPitch);
}

void STDMETHODCALLTYPE hookedCopyResource(
    ID3D11DeviceContext* self, ID3D11Resource* dstResource,
    ID3D11Resource* srcResource) {
  D3D11_TEXTURE2D_DESC desc;
  if (as512Texture(dstResource, desc))
    reportIfNew(kEntryCopyResource, dstResource, callerRva(), desc, nullptr);
  originalCopyResource(self, dstResource, srcResource);
}

void STDMETHODCALLTYPE hookedCopySubresourceRegion(
    ID3D11DeviceContext* self, ID3D11Resource* dstResource,
    UINT dstSubresource, UINT dstX, UINT dstY, UINT dstZ,
    ID3D11Resource* srcResource, UINT srcSubresource,
    const D3D11_BOX* srcBox) {
  D3D11_TEXTURE2D_DESC desc;
  if (as512Texture(dstResource, desc))
    reportIfNew(kEntryCopySubresourceRegion, dstResource, callerRva(), desc,
                nullptr);
  originalCopySubresourceRegion(self, dstResource, dstSubresource, dstX, dstY,
                                dstZ, srcResource, srcSubresource, srcBox);
}

// Same ordering discipline as main.cpp's hook installs: every MH_CreateHook is
// attempted before any MH_EnableHook, so a failure partway through leaves
// every target un-enabled (pass-through) rather than some hooks live and
// others not.
void installHooks(ID3D11DeviceContext* context) {
  auto** vtable = *reinterpret_cast<void***>(context);
  void* mapTarget = vtable[14];
  void* copySubresourceRegionTarget = vtable[46];
  void* copyResourceTarget = vtable[47];
  void* updateSubresourceTarget = vtable[48];

  const bool created =
    MH_CreateHook(mapTarget, reinterpret_cast<void*>(&hookedMap),
                  reinterpret_cast<void**>(&originalMap)) == MH_OK &&
    MH_CreateHook(copySubresourceRegionTarget,
                  reinterpret_cast<void*>(&hookedCopySubresourceRegion),
                  reinterpret_cast<void**>(&originalCopySubresourceRegion))
      == MH_OK &&
    MH_CreateHook(copyResourceTarget,
                  reinterpret_cast<void*>(&hookedCopyResource),
                  reinterpret_cast<void**>(&originalCopyResource)) == MH_OK &&
    MH_CreateHook(updateSubresourceTarget,
                  reinterpret_cast<void*>(&hookedUpdateSubresource),
                  reinterpret_cast<void**>(&originalUpdateSubresource))
      == MH_OK;
  if (!created) {
    log("D3D11PROBE: MH_CreateHook failed, installing nothing");
    return;
  }

  const bool enabled =
    MH_EnableHook(mapTarget) == MH_OK &&
    MH_EnableHook(copySubresourceRegionTarget) == MH_OK &&
    MH_EnableHook(copyResourceTarget) == MH_OK &&
    MH_EnableHook(updateSubresourceTarget) == MH_OK;
  if (!enabled) {
    log("D3D11PROBE: MH_EnableHook failed, installing nothing");
    return;
  }

  g_installed = true;
  log("D3D11PROBE: installed (Map/UpdateSubresource/CopyResource/"
      "CopySubresourceRegion) -- watching for D3D11-level writes to any"
      " 512x512 texture");
}

}  // namespace

void initializeD3D11WriteProbe(ID3D11DeviceContext* context) {
  static bool done = false;
  if (done || !context || !featureEnabled(Feature::D3D11WriteProbe))
    return;
  installHooks(context);
  // Only latched on success, same as main.cpp's hookPresent: a failed attempt
  // leaves the door open for a later context to try again instead of
  // permanently reporting nothing.
  done = g_installed;
}

void d3d11WriteProbeFrameTick() {
  if (!g_installed)
    return;
  static uint64_t frame = 0;
  ++frame;
  // The early one exists so a run confirms within seconds that the probe is
  // live, rather than looking silent until the first interval elapses.
  if (frame != 60 && frame % kReportInterval != 0)
    return;

  const uint64_t writes = g_totalWrites.load(std::memory_order_relaxed);
  size_t distinctSites = 0;
  {
    std::lock_guard lock(g_sitesMutex);
    distinctSites = g_sites.size();
  }

  if (writes == 0) {
    log("D3D11PROBE frame=", frame, " writesTo512x512=0"
        " (no D3D11-level writes to any 512x512 texture observed)");
  } else {
    log("D3D11PROBE frame=", frame, " writesTo512x512=", writes,
        " distinctSites=", distinctSites,
        g_sitesOverflowed ? " (site table capped; totals remain exact)" : "");
  }
}

}  // namespace atfix
