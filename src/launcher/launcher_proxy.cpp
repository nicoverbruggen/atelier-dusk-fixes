// SPDX-License-Identifier: MIT
//
// 32-bit MSIMG32 proxy for the Dusk front-ends. Ported from the Arland project's
// src/launcher/launcher_proxy.cpp, which is where every load-bearing decision below was
// learned; this file changes the names it matches on and nothing about the
// mechanism.
//
// All six Dusk front-ends are 32-bit and statically import MSIMG32, exactly as
// the Arland ones do, so one DLL is loaded into each and does a different job
// in each:
//
//   Atelier_<Game>Launcher.exe -> start dusk-fix-launcher.exe instead, if it is
//                                 installed, or the game itself when
//                                 dusk-fix.ini asks for that with
//                                 [Launcher] SkipLauncher
//   Atelier_<Game>Env.exe      -> nothing; the settings editor is only forwarded
//                                 to. It writes the game's own Setting.ini, and
//                                 Ayesha reads its resolution straight out of
//                                 that file with no clamping of its own
//                                 so there is nothing here worth patching.
//
// Everything else that loads it just gets the two forwarded GDI entry points.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "aspect_fit.h"

namespace {

using PFN_AlphaBlend = BOOL (WINAPI *)(
  HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
using PFN_TransparentBlt = BOOL (WINAPI *)(
  HDC, int, int, int, int, HDC, int, int, int, int, UINT);
INIT_ONCE g_msimg32Init = INIT_ONCE_STATIC_INIT;
PFN_AlphaBlend g_alphaBlend = nullptr;
PFN_TransparentBlt g_transparentBlt = nullptr;

// Returns the host's file name, or false if it could not be determined. Six
// executables load this DLL and only three of them are redirected, so telling
// them apart is the first thing armRedirect does.
bool hostExeName(std::array<wchar_t, 32768>& path, const wchar_t** name) {
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), path.size());
  if (!length || length == path.size())
    return false;
  *name = path.data();
  for (const wchar_t* cursor = path.data(); *cursor; cursor++) {
    if (*cursor == L'\\' || *cursor == L'/')
      *name = cursor + 1;
  }
  return true;
}

// ---- the launcher redirect -------------------------------------------------
//
// Steam runs Atelier_<Game>Launcher.exe. When our own launcher is installed
// beside it we run that instead, and the stock one never puts a window on
// screen, so a plain drop-in install replaces it with no extra steps.
//
// [Launcher] SkipLauncher in dusk-fix.ini turns that into a straight start of
// the game: neither front-end appears and the configured settings are used as
// they stand. Only the destination changes -- everything below about when the
// substitution happens and how long this process lives applies unchanged, which
// is what keeps the Steam session, the overlay and Steam Input attached either
// way.
//
// Two things about *when* and *for how long* this happens are load-bearing, and
// both were learned the hard way in the Arland project:
//
//  - It must not happen in DllMain. This DLL is a static import of the
//    launcher, so its process attach runs before the executable's entry point
//    and before anything injected into the process has finished setting itself
//    up -- including Steam's overlay, which hooks process creation to follow
//    the game into child processes. Starting our launcher from there
//    produced a child Steam knew nothing about: no overlay, no frame-rate
//    counter, and no Steam Input, which is what makes a DualSense work at all
//    when Steam is handling it. So the redirect is armed here and runs at the
//    executable's entry point instead, by which time the process is fully
//    assembled.
//
//    The same rule applies to DECIDING what to start. Reading which target the
//    settings ask for means profile reads, file-attribute queries and, on the
//    KTGL games, profile writes, and DllMain holds the loader lock. All of it
//    therefore happens at the entry point too, in resolveStartTarget.
//    armRedirect verifies the entry window and splices; nothing else.
//
//  - The stock launcher process must stay alive while ours is open, rather than
//    being terminated the moment its replacement starts. It is the process
//    Steam launched and is counting, and the game is started from our launcher
//    underneath it. Waiting costs nothing -- this process has no window and no
//    work of its own once its entry point belongs to us.
//
// Nothing here is destructive if the install is partial. The entry point is
// restored before anything is decided, so with no dusk-fix-launcher.exe next to
// it the stock launcher runs from its own unmodified entry bytes.
//
// DUSK_NO_REDIRECT stands the redirect down. Our launcher sets it on the
// original launcher and settings editor when its own buttons open them, which
// is what stops those buttons from being bounced straight back here.

// What the redirect starts: our launcher normally, the game itself when
// SkipLauncher is set. Both are resolved at the entry point rather than at
// arm time; see redirectedEntryPoint.
std::array<wchar_t, 32768> g_startTarget = { };
bool g_startsGame = false;
// The row armRedirect matched on. Points into SupportedGames, which is a
// namespace-scope constant, so it stays valid for the life of the process.
// redirectedEntryPoint needs it and cannot re-derive it: by then the host name
// has already served its purpose.
const struct DuskGame* g_game = nullptr;
std::array<wchar_t, 32768> g_gameDirectory = { };
std::uint8_t* g_entryPoint = nullptr;
std::array<std::uint8_t, 5> g_entryOriginal = { };

// `directory` + `name`, where g_gameDirectory keeps its trailing backslash, so
// this is a plain concatenation. False if the result does not fit.
bool pathInGameDirectory(const wchar_t* name, std::array<wchar_t, 32768>& out) {
  const std::size_t directoryLength =
    static_cast<std::size_t>(lstrlenW(g_gameDirectory.data()));
  const std::size_t nameLength = static_cast<std::size_t>(lstrlenW(name));
  if (directoryLength + nameLength + 1 > out.size())
    return false;
  std::memcpy(out.data(), g_gameDirectory.data(),
    directoryLength * sizeof(wchar_t));
  std::memcpy(out.data() + directoryLength, name,
    (nameLength + 1) * sizeof(wchar_t));
  return true;
}

// [Launcher] SkipLauncher in dusk-fix.ini, read the way the DLL's config.cpp
// reads every other boolean (t/T/1/y/Y is true). Read wide, because the ini
// sits in the game folder and a Steam library path can hold characters the ANSI
// code page cannot represent.
//
// Deliberately read-only: unlike the DLL's own options this one is never seeded
// when it is absent. Creating dusk-fix.ini from here would leave config.cpp's
// first-use seeding thinking the file already exists, and the rest of the
// defaults would never be written into it.
bool skipLauncherRequested() {
  std::array<wchar_t, 32768> ini = { };
  if (!pathInGameDirectory(L"dusk-fix.ini", ini))
    return false;
  std::array<wchar_t, 16> value = { };
  GetPrivateProfileStringW(L"Launcher", L"SkipLauncher", L"false",
    value.data(), static_cast<DWORD>(value.size()), ini.data());
  return value[0] == L't' || value[0] == L'T' || value[0] == L'1' ||
         value[0] == L'y' || value[0] == L'Y';
}

// [Launcher] AutoResolution, and what it costs to honour it here.
//
// Auto means "present at the display's resolution". The mod launcher resolves
// it when it saves, so every start through that window already follows the
// display. SkipLauncher is the gap: that window never opens, so the number in
// Setting.ini keeps whatever the display happened to be when it was last
// resolved, and a display change is never noticed.
//
// The Arland mod closes the same gap in its DLL, which overrides the swap chain
// at device creation. That does not port here: Arland can present at a size the
// game did not choose because it also carries a render-to-display fit pass to
// bridge the two (supersample.cpp). Dusk has no such pass, so a swap chain that
// disagrees with the size the engine picked for its own targets would put the
// scene in a corner of the backbuffer. Writing the game's own field instead is
// both safer and more honest: the game reads it itself, so the resolution is
// real rather than something the mod imposes on top.
//
// Read-only on dusk-fix.ini for the reason given on skipLauncherRequested; only
// Setting.ini is written, and only when it already exists.
bool autoResolutionRequested() {
  std::array<wchar_t, 32768> ini = { };
  if (!pathInGameDirectory(L"dusk-fix.ini", ini))
    return false;
  std::array<wchar_t, 16> value = { };
  GetPrivateProfileStringW(L"Launcher", L"AutoResolution", L"false",
    value.data(), static_cast<DWORD>(value.size()), ini.data());
  return value[0] == L't' || value[0] == L'T' || value[0] == L'1' ||
         value[0] == L'y' || value[0] == L'Y';
}

void applyAutoResolution(bool ssaaScalesGameIni) {
  if (!autoResolutionRequested())
    return;
  DEVMODEW mode = { };
  mode.dmSize = sizeof(mode);
  unsigned width = 0, height = 0;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmPelsWidth && mode.dmPelsHeight) {
    width = mode.dmPelsWidth;
    height = mode.dmPelsHeight;
  } else {
    const int cx = GetSystemMetrics(SM_CXSCREEN);
    const int cy = GetSystemMetrics(SM_CYSCREEN);
    if (cx > 0 && cy > 0) { width = unsigned(cx); height = unsigned(cy); }
  }
  if (!width || !height)
    return;
  std::array<wchar_t, 32768> settings = { };
  if (!pathInGameDirectory(L"Setting.ini", settings) ||
      GetFileAttributesW(settings.data()) == INVALID_FILE_ATTRIBUTES)
    return;

  // A 16:9 base: always on PhyreEngine, and on KTGL only in a window, where the
  // back buffer then comes out 16:9 and that engine's fit pass declines on its
  // own. See aspect_fit.h.
  const bool windowed =
    GetPrivateProfileIntW(L"Window", L"FullScreen", 1, settings.data()) == 0;
  if (!ssaaScalesGameIni || windowed)
    atfix::fitRenderToSixteenNine(&width, &height);

  // KTGL sizes its scene from Setting.ini itself. The GUI therefore writes
  // base x SSAA there and keeps the base in dusk-fix.ini for the swap-chain
  // clamp. Re-resolving Auto used to overwrite Setting.ini with the bare
  // desktop on every redirected start, silently disabling the larger scene.
  // Always derive from the desktop base and the saved factor -- never from the
  // already-multiplied Setting.ini value -- so repeated starts are idempotent.
  unsigned renderWidth = width;
  unsigned renderHeight = height;
  unsigned percent = 100;
  std::array<wchar_t, 32768> ini = { };
  if (ssaaScalesGameIni && pathInGameDirectory(L"dusk-fix.ini", ini)) {
    const int configured = GetPrivateProfileIntW(
      L"Rendering", L"Supersampling", 100, ini.data());
    const unsigned wanted =
      configured == 125 || configured == 150 || configured == 200 ||
      configured == 300 || configured == 400
        ? unsigned(configured) : 100u;
    constexpr unsigned factors[] = { 400, 300, 200, 150, 125 };
    constexpr uint64_t maxWidth = 7680;
    constexpr uint64_t maxHeight = 4320;
    for (unsigned candidate : factors) {
      if (candidate > wanted)
        continue;
      const uint64_t candidateWidth = uint64_t(width) * candidate / 100;
      const uint64_t candidateHeight = uint64_t(height) * candidate / 100;
      if (candidateWidth <= maxWidth && candidateHeight <= maxHeight) {
        percent = candidate;
        renderWidth = unsigned(candidateWidth) & ~1u;
        renderHeight = unsigned(candidateHeight) & ~1u;
        break;
      }
    }
  }

  // A refused write leaves that field at whatever it already held, which is a
  // resolution the game can still use, so each one stands or falls on its own
  // and none of them is worth abandoning the rest for.
  wchar_t value[16] = { };
  wsprintfW(value, L"%u", renderWidth);
  WritePrivateProfileStringW(L"Graphics", L"ScreenWidth", value,
                             settings.data());
  wsprintfW(value, L"%u", renderHeight);
  WritePrivateProfileStringW(L"Graphics", L"ScreenHeight", value,
                             settings.data());

  if (ssaaScalesGameIni && ini[0]) {
    if (percent > 100) {
      wsprintfW(value, L"%u", width);
      WritePrivateProfileStringW(
        L"Rendering", L"DisplayWidth", value, ini.data());
      wsprintfW(value, L"%u", height);
      WritePrivateProfileStringW(
        L"Rendering", L"DisplayHeight", value, ini.data());
    } else {
      WritePrivateProfileStringW(
        L"Rendering", L"DisplayWidth", nullptr, ini.data());
      WritePrivateProfileStringW(
        L"Rendering", L"DisplayHeight", nullptr, ini.data());
    }
    const int configured = GetPrivateProfileIntW(
      L"Rendering", L"Supersampling", 100, ini.data());
    if (configured != int(percent)) {
      wsprintfW(value, L"%u", percent);
      WritePrivateProfileStringW(
        L"Rendering", L"Supersampling", value, ini.data());
    }
  }
}

struct DuskGame {
  const wchar_t* launcher;
  const wchar_t* english;
  const wchar_t* multilingual;
  bool ssaaScalesGameIni;
  std::array<std::uint8_t, 17> launcherEntryExpected;
};

// The three games. Each ships an English build and a multilingual one carrying
// Japanese, Simplified Chinese and Traditional Chinese, normally installed side
// by side, and each game folder carries exactly one of these launchers.
//
// Unlike the Arland trilogy, whose three games share one ArlandDXLauncher.exe
// binary, the Dusk launchers are per-game files. They are built from the same
// source -- identical `.text` VirtualSize (0x14f4f4) and identical entry-point
// RVA (0x1216f2) across all three -- but their bytes differ. Each row therefore
// carries its own verified 17-byte entry window; a file name and a header RVA
// alone are not authority to overwrite executable code. The last dword is an
// absolute address, and all three PE relocation tables name it as HIGHLOW at
// entry+13, so the comparison applies the loader's ASLR delta first.
constexpr std::array<DuskGame, 3> SupportedGames = {{
  { L"Atelier_AyeshaLauncher.exe",
    L"Atelier_Ayesha_EN.exe",           L"Atelier_Ayesha.exe",
    false,
    { 0xe8,0x7f,0xe6,0x00,0x00,0xe9,0x00,0x00,
      0x00,0x00,0x6a,0x14,0x68,0xb8,0xaf,0x59,0x00 } },
  { L"Atelier_Escha_and_LogyLauncher.exe",
    L"Atelier_Escha_and_Logy_EN.exe",   L"Atelier_Escha_and_Logy.exe",
    true,
    { 0xe8,0x7f,0xe6,0x00,0x00,0xe9,0x00,0x00,
      0x00,0x00,0x6a,0x14,0x68,0xf8,0xaf,0x59,0x00 } },
  { L"Atelier_ShallieLauncher.exe",
    L"Atelier_Shallie_EN.exe",          L"Atelier_Shallie.exe",
    true,
    { 0xe8,0x7f,0xe6,0x00,0x00,0xe9,0x00,0x00,
      0x00,0x00,0x6a,0x14,0x68,0xb8,0xaf,0x59,0x00 } },
}};

constexpr std::size_t kLauncherEntryRelocationOffset = 13;
constexpr std::uint32_t kLauncherPreferredImageBase = 0x00400000;

template<std::size_t N>
void relocateEntryWindow(std::uint8_t* loadedBase,
                         std::array<std::uint8_t, N>& expected) {
  static_assert(N >= kLauncherEntryRelocationOffset + sizeof(std::uint32_t));
  std::uint32_t absolute = 0;
  std::memcpy(&absolute, expected.data() + kLauncherEntryRelocationOffset,
              sizeof(absolute));
  // PE32 HIGHLOW relocation arithmetic is modulo 2^32. The preferred base is
  // the one verified in all three files, not the loaded header's ImageBase:
  // Wine rewrites that field to the actual base and would make the delta zero.
  absolute += static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(loadedBase)) -
              kLauncherPreferredImageBase;
  std::memcpy(expected.data() + kLauncherEntryRelocationOffset, &absolute,
              sizeof(absolute));
}

// Which executable a straight start runs, decided exactly as Koei Tecmo's own
// launcher decides it: [Lang] Language in Setting.ini selects the multilingual
// build for 1 (Japanese), 3 (Simplified Chinese) and 4 (Traditional Chinese),
// and the English build for 2 or for anything unrecognized. The two builds do
// not each carry every language, so this matters.
//
// That mapping is read out of the stock launcher rather than assumed from the
// Arland convention it happens to match. `Atelier_AyeshaLauncher.exe` compares
// the parsed value against the string constants "1", "2", "3", "4" at
// 0x551ea4/ea8/eac/eb0 and branches to a per-language executable name; the "2"
// arm and the fall-through arm are the same branch, which is where the
// English default comes from. The other two launchers carry the identical
// table at the identical file offset with their own names.
//
// The comparison below is against the whole value for the same reason: the
// stock launcher compares whole strings, so "10" is unrecognized there and
// falls to English, where a first-character test would read it as Japanese.
//
// `game` is the row the host launcher identified, so this never has to guess
// which title it is in. If the build the language calls for is not installed
// the other one is used rather than starting nothing.
bool resolveGameExecutable(const DuskGame& game,
                           std::array<wchar_t, 32768>& out) {
  std::array<wchar_t, 16> language = { };
  std::array<wchar_t, 32768> settings = { };
  if (pathInGameDirectory(L"Setting.ini", settings))
    GetPrivateProfileStringW(L"Lang", L"Language", L"2", language.data(),
      static_cast<DWORD>(language.size()), settings.data());
  const bool english = lstrcmpW(language.data(), L"1") != 0 &&
                       lstrcmpW(language.data(), L"3") != 0 &&
                       lstrcmpW(language.data(), L"4") != 0;

  const wchar_t* candidates[2] = {
    english ? game.english : game.multilingual,
    english ? game.multilingual : game.english,
  };
  for (const wchar_t* name : candidates) {
    if (pathInGameDirectory(name, out) &&
        GetFileAttributesW(out.data()) != INVALID_FILE_ATTRIBUTES)
      return true;
  }
  out[0] = L'\0';
  return false;
}

// Put the executable's own five entry bytes back, so the image is the one that
// shipped. Called first thing from redirectedEntryPoint, before anything that
// could fail, which is what makes every path below it able to fall back to the
// stock launcher without a second protection change.
bool restoreEntryPoint() {
  DWORD oldProtect = 0;
  if (!VirtualProtect(g_entryPoint, g_entryOriginal.size(),
      PAGE_EXECUTE_READWRITE, &oldProtect))
    return false;
  std::memcpy(g_entryPoint, g_entryOriginal.data(), g_entryOriginal.size());
  FlushInstructionCache(GetCurrentProcess(), g_entryPoint,
    g_entryOriginal.size());
  // Unchecked, and it has to stay that way: the original bytes are already
  // back, so the stock launcher runs correctly whether or not the page returns
  // to its old protection. Refusing to call it over a left-writable page would
  // turn a cosmetic failure into a launcher that does nothing.
  DWORD ignored = 0;
  VirtualProtect(g_entryPoint, g_entryOriginal.size(), oldProtect, &ignored);
  return true;
}

// Run the launcher as if the mod were not installed. Only valid once
// restoreEntryPoint has succeeded: while the jump is still there this would
// re-enter redirectedEntryPoint and recurse to stack failure.
void runOriginalEntryPoint() {
  reinterpret_cast<void (*)()>(g_entryPoint)();
}

// Which target to start, resolved here rather than at arm time.
//
// ALL OF THIS IS FILE WORK: two profile reads and up to three file-attribute
// queries, and on the KTGL games applyAutoResolution adds a display
// enumeration and five profile writes. armRedirect runs from DllMain, so doing
// it there put every one of those under the loader lock. Nothing about them
// needs to happen that early -- their only ordering requirement is that they
// precede the target starting, and this does.
bool resolveStartTarget() {
  g_startsGame = skipLauncherRequested();
  // Before either target starts. With SkipLauncher this is the only chance to
  // follow the display at all; without it the window is about to re-resolve the
  // same value anyway, so doing it here as well keeps one code path rather than
  // two.
  applyAutoResolution(g_game && g_game->ssaaScalesGameIni);
  if (g_startsGame)
    return g_game && resolveGameExecutable(*g_game, g_startTarget);
  return pathInGameDirectory(L"dusk-fix-launcher.exe", g_startTarget) &&
         GetFileAttributesW(g_startTarget.data()) != INVALID_FILE_ATTRIBUTES;
}

// Stands in for the launcher's entry point once the redirect is armed.
void redirectedEntryPoint() {
  // RESTORED FIRST, before anything that can fail or take time. Two things
  // follow from that. The image spends the shortest possible time carrying the
  // jump, and from here on the stock launcher is reachable by a plain call --
  // there is no way back into this function, so a failure below is a fallback
  // rather than a recursion.
  //
  // Exiting is the only thing left if the restore itself fails, because the
  // jump is then still live and calling it comes straight back here.
  if (!restoreEntryPoint())
    ExitProcess(1);

  // No target: the mod is partially installed, or the game folder does not hold
  // what SkipLauncher asked for. The launcher comes up exactly as it would
  // without the mod.
  if (!resolveStartTarget()) {
    runOriginalEntryPoint();
    return;
  }

  STARTUPINFOW startup = { };
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = { };
  // Both targets are 64-bit and this proxy is 32-bit; CreateProcess spans that
  // difference, and the child inherits our environment either way -- which is
  // how the Steam variables reach the game and stop it restarting itself
  // through Steam. Starting the game from here rather than from our launcher
  // makes it a child of this process instead of a grandchild, which is the
  // same relationship our launcher gives it and the one Steam follows.
  if (!CreateProcessW(g_startTarget.data(), nullptr, nullptr, nullptr, FALSE,
      0, nullptr, g_gameDirectory.data(), &startup, &process)) {
    runOriginalEntryPoint();
    return;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  CloseHandle(process.hProcess);
  ExitProcess(0);
}

// Point the executable's entry point at redirectedEntryPoint. Returns false
// with the image untouched if anything does not look as expected.
bool armRedirect() {
  std::array<wchar_t, 32768> path = { };
  const wchar_t* name = nullptr;
  if (!hostExeName(path, &name))
    return false;

  const DuskGame* game = nullptr;
  for (const DuskGame& candidate : SupportedGames) {
    if (_wcsicmp(name, candidate.launcher) == 0) {
      game = &candidate;
      break;
    }
  }
  // Every other host, all three Atelier_<Game>Env.exe settings editors
  // included, is left completely alone.
  if (!game)
    return false;

  if (GetEnvironmentVariableW(L"DUSK_NO_REDIRECT", nullptr, 0) != 0 ||
      GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
    return false;
  }

  // Directory of the launcher, which is also the game folder our launcher is
  // dropped into. `name` points into `path`, so truncating there leaves the
  // directory with its trailing backslash.
  const std::size_t directoryLength = static_cast<std::size_t>(name - path.data());
  std::memcpy(g_gameDirectory.data(), path.data(),
    directoryLength * sizeof(wchar_t));

  // Where the redirect goes. SkipLauncher asks for the game itself; without it
  // (the default) this is our launcher.
  // Where the redirect goes is decided at the entry point, not here. See
  // resolveStartTarget: all of it is profile and file-system work, and this
  // function runs under the loader lock.
  g_game = game;

  auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
  if (!base)
    return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  const auto* nt =
    reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      !nt->OptionalHeader.AddressOfEntryPoint ||
      nt->OptionalHeader.SizeOfImage < game->launcherEntryExpected.size() ||
      nt->OptionalHeader.AddressOfEntryPoint >
        nt->OptionalHeader.SizeOfImage - game->launcherEntryExpected.size())
    return false;

  // The header locates the entry, and this title's loader-adjusted shipped byte
  // window identifies it. A future build is deliberately left untouched until
  // its entry bytes and relocation are measured and added to the row.
  g_entryPoint = base + nt->OptionalHeader.AddressOfEntryPoint;
  auto expectedEntry = game->launcherEntryExpected;
  relocateEntryWindow(base, expectedEntry);
  if (std::memcmp(g_entryPoint, expectedEntry.data(), expectedEntry.size())) {
    g_entryPoint = nullptr;
    return false;
  }
  std::memcpy(g_entryOriginal.data(), g_entryPoint, g_entryOriginal.size());

  DWORD oldProtect = 0;
  if (!VirtualProtect(g_entryPoint, g_entryOriginal.size(),
      PAGE_EXECUTE_READWRITE, &oldProtect)) {
    return false;
  }
  g_entryPoint[0] = 0xe9;
  const std::int32_t delta = static_cast<std::int32_t>(
    reinterpret_cast<std::uint8_t*>(&redirectedEntryPoint) - (g_entryPoint + 5));
  std::memcpy(g_entryPoint + 1, &delta, sizeof(delta));
  DWORD ignored = 0;
  VirtualProtect(g_entryPoint, g_entryOriginal.size(), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), g_entryPoint,
    g_entryOriginal.size());
  return true;
}

BOOL CALLBACK loadSystemMsimg32(PINIT_ONCE, PVOID, PVOID*) {
  std::array<wchar_t, MAX_PATH> path = { };
  const UINT length = GetSystemDirectoryW(path.data(), path.size());
  if (!length || length + 14 >= path.size())
    return TRUE;
  std::memcpy(path.data() + length, L"\\msimg32.dll", 13 * sizeof(wchar_t));
  HMODULE module = LoadLibraryW(path.data());
  if (module) {
    g_alphaBlend = reinterpret_cast<PFN_AlphaBlend>(
      GetProcAddress(module, "AlphaBlend"));
    g_transparentBlt = reinterpret_cast<PFN_TransparentBlt>(
      GetProcAddress(module, "TransparentBlt"));
  }
  return TRUE;
}

} // namespace

extern "C" BOOL WINAPI AlphaBlend(
    HDC dst, int dstX, int dstY, int dstWidth, int dstHeight,
    HDC src, int srcX, int srcY, int srcWidth, int srcHeight,
    BLENDFUNCTION blend) {
  InitOnceExecuteOnce(&g_msimg32Init, loadSystemMsimg32, nullptr, nullptr);
  return g_alphaBlend && g_alphaBlend(dst, dstX, dstY, dstWidth, dstHeight,
    src, srcX, srcY, srcWidth, srcHeight, blend);
}

extern "C" BOOL WINAPI TransparentBlt(
    HDC dst, int dstX, int dstY, int dstWidth, int dstHeight,
    HDC src, int srcX, int srcY, int srcWidth, int srcHeight,
    UINT transparent) {
  InitOnceExecuteOnce(&g_msimg32Init, loadSystemMsimg32, nullptr, nullptr);
  return g_transparentBlt && g_transparentBlt(
    dst, dstX, dstY, dstWidth, dstHeight,
    src, srcX, srcY, srcWidth, srcHeight, transparent);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    // Only armed here; it runs at the executable's entry point, once the
    // process (Steam's injections included) is fully assembled.
    armRedirect();
  }
  return TRUE;
}
