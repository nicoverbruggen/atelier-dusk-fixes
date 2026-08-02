// SPDX-License-Identifier: MIT
//
// dusk-fix-launcher.exe: the mod's launcher. It edits the settings the mod has
// and the game's own, and starts the game; it is what msimg32.dll opens in
// place of Koei Tecmo's launcher when the game is started from Steam.
//
// Structured to match the Arland project's src/config_gui/main.cpp deliberately
// and closely: the same tab strip with the game name on it, the same
// cursor-driven Layout with measured note heights, the same bottom button row
// with Play on the left and the skip-launcher checkbox beside it, the same
// About page. Someone who has used one should not have to learn the other.
//
// It carries three of Arland's four pages. Image Quality is absent because this
// mod has no image-quality options at all -- no MSAA, supersampling,
// anisotropic filtering, shadow maps or SMAA -- and an empty tab would be worse
// than a missing one. Debug is absent for the same reason: there are no
// developer views to reach, and the diagnostics that do exist are environment
// switches on purpose (WORK_DOC.md, "Configuration: dusk-fix.ini").
//
// Two files are edited, and the split matters:
//
//   Setting.ini   the game's own, which it reads by itself. Resolution lives
//                 here and nowhere else -- Ayesha accepts any value in it and
//                 validates nothing but a negative number (WORK_DOC.md,
//                 "Ayesha's resolution path"), so the mod deliberately has no
//                 resolution setting of its own to duplicate it with.
//   dusk-fix.ini  the mod's, parsed by src/core/config.cpp. The same
//                 Get/WritePrivateProfileString API is used here so the
//                 on-disk format matches, and only known keys are touched, so
//                 anything else already in either file is preserved.
//
// It configures whichever game folder it is run from, which is not always the
// folder it lives in: see resolveGameFolder.
//
// No external dependencies: plain Win32 common controls, GUI subsystem. The
// modern look is the ComCtl32 v6 manifest in dusk-fix-launcher.rc plus the
// system UI font; there is no toolkit here.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <vssym32.h>

// SysLink's self-measuring message. The Windows SDK defines it as an alias for
// LM_GETIDEALHEIGHT; MinGW's commctrl.h carries neither name, so it is spelled
// out here rather than losing the measurement on the cross-build.
#ifndef LM_GETIDEALSIZE
#define LM_GETIDEALSIZE (WM_USER + 0x301)
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

enum : int {
  IDC_TABS = 1001,
  IDC_RES,
  IDC_WINMODE,
  IDC_LANG,
  IDC_OUTLINE,
  IDC_SKIPLAUNCHER,
  IDC_START,
  IDC_OPENLAUNCHER,   // Koei Tecmo's own launcher
  IDC_OPENENV,        // Koei Tecmo's own settings editor
  IDC_PLAYVANILLA,    // the game with the mod stood down
  IDC_RESET,
  IDC_CLOSE,
  IDC_REPOLINK,
};

// Shown in full and opened on click, so it is the one string in this window
// that has to be right: it is where the window sends people.
const wchar_t* const kRepositoryUrl =
  L"https://github.com/nicoverbruggen/atelier-dusk-fixes";

const int kPageCount = 3;   // Display, Game, About

// ---- the games -------------------------------------------------------------

// Each game ships two executables and the language decides which one runs: the
// `_EN` build is English, the other is the multilingual build carrying
// Japanese, Simplified Chinese and Traditional Chinese. Both are normally
// installed side by side, so which is present is not the question -- which to
// start is (see gameExeForLanguage).
//
// There is deliberately no engine flag here any more. Every mod fix is on by
// default and has no control in this window, so nothing it shows depends on
// which engine the game runs: the window is identical for all three, and the
// per-game difference lives entirely in the DLL's capability matrix.
struct Game {
  const char*    english;
  const char*    multilingual;
  const char*    stockLauncher;
  const char*    stockEnv;
  const wchar_t* name;
};

const Game kGames[] = {
  { "Atelier_Ayesha_EN.exe", "Atelier_Ayesha.exe",
    "Atelier_AyeshaLauncher.exe", "Atelier_AyeshaEnv.exe",
    L"Atelier Ayesha DX" },
  { "Atelier_Escha_and_Logy_EN.exe", "Atelier_Escha_and_Logy.exe",
    "Atelier_Escha_and_LogyLauncher.exe", "Atelier_Escha_and_LogyEnv.exe",
    L"Atelier Escha & Logy DX" },
  { "Atelier_Shallie_EN.exe", "Atelier_Shallie.exe",
    "Atelier_ShallieLauncher.exe", "Atelier_ShallieEnv.exe",
    L"Atelier Shallie DX" },
};
const int kGameCount = 3;

char g_iniPath[MAX_PATH] = {};       // dusk-fix.ini, in the game folder
char g_settingsPath[MAX_PATH] = {};  // the game's own Setting.ini
char g_gameExePath[MAX_PATH] = {};   // an installed game exe, for the icon
char g_gameDir[MAX_PATH] = {};       // its folder, used as the working directory
const wchar_t* g_gameName = nullptr;
int g_game = -1;                     // index into kGames, -1 when none found


// ---- combo box contents ----------------------------------------------------

struct ComboItem {
  const wchar_t* label;
  const char*    value;
};

struct Resolution { unsigned width; unsigned height; };

// Resolutions. This list is ours and is not filtered through Windows' reported
// display modes, which is the whole point: the games take any value, and the
// stock settings editor's list is what hides the useful ones on a high-DPI
// handheld or in docked use. The current desktop mode is appended at load time
// if it is not already here, as is whatever the ini already holds, so opening
// this window can never silently change a resolution it did not offer.
//
// Note this deliberately does NOT drop modes larger than the display, which the
// Arland launcher does. Ayesha windowed will happily create a swap chain bigger
// than the screen, and that is how the 1440p render-target census was measured
// on a 1080p panel; filtering by the display maximum would have removed the one
// entry that made it possible. The cost is that a fullscreen selection above
// the panel's size is offered and will not do anything useful.
const Resolution kResolutions[] = {
  { 1280,  720 }, { 1366,  768 }, { 1600,  900 }, { 1920, 1080 },
  { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 },
};

// Index 0 of the live list is always Auto, carried as a 0x0 sentinel exactly as
// the Arland launcher carries it.
//
// The two projects have to resolve it in different places, though. Arland
// leaves its ini keys blank and its DLL decides what they mean when the device
// is created, so Auto there follows the display even if it changes between
// launches. Dusk writes a literal number into the game's own Setting.ini,
// because the game reads that field itself and this mod deliberately adds no
// resolution override to duplicate it -- and "Auto" is not something the game's
// integer parse can be handed. So Auto is resolved here, at save time, and the
// choice is remembered in dusk-fix.ini so reopening the window still shows
// Auto rather than the number it happened to resolve to last time.
constexpr Resolution kAutoResolution = { 0, 0 };
const int kResolutionCount = int(sizeof(kResolutions) / sizeof(kResolutions[0]));

const ComboItem kWindowModeItems[] = {
  { L"Windowed",   "0" },
  { L"Fullscreen", "1" },
};
const int kWindowModeCount = 2;

// The values Koei Tecmo's launcher compares against, and the build each one
// starts. Verified from the stock launcher's own string-compare chain rather
// than assumed; see WORK_DOC.md, "How the stock launcher picks the game build".
const ComboItem kLangItems[] = {
  { L"Japanese",            "1" },
  { L"English",             "2" },
  { L"Simplified Chinese",  "3" },
  { L"Traditional Chinese", "4" },
};
const int kLangCount = 4;

// ---- controls --------------------------------------------------------------

HWND g_hTabs = nullptr;
HWND g_hGameLabel = nullptr;   // sits on the tab strip; painted transparent
HWND g_hRes = nullptr, g_hWinMode = nullptr, g_hLang = nullptr;
HWND g_hOutline = nullptr;
HWND g_hSkipLauncher = nullptr;
HWND g_hStart = nullptr, g_hOpenLauncher = nullptr, g_hOpenEnv = nullptr;
HWND g_hPlayVanilla = nullptr;
HWND g_hRepoLink = nullptr;

HWND g_pageCtrls[kPageCount][40] = {};
int  g_pageCount[kPageCount] = {};
HWND g_hDesc[32] = {};   // greyed notes; drawn in g_secondaryText
int  g_descCount = 0;

HFONT g_uiFont = nullptr;
HFONT g_headingFont = nullptr;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_pageBrush = nullptr;
int g_lineHeight = 0;    // one line of g_uiFont, device pixels

RECT g_pageRect = {};    // where the tab pages may draw, in parent coordinates
int g_contentBottom = 0; // how far the tallest page got

COLORREF g_windowBack = RGB(255, 255, 255);
COLORREF g_pageBack   = RGB(255, 255, 255);
COLORREF g_text       = RGB(0, 0, 0);
COLORREF g_secondaryText = RGB(102, 102, 102);

bool nativeStyling();
LRESULT CALLBACK TabProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

// The base layout, in logical pixels. Sized for 720p at 100% so chooseScale
// always has an enlargement it can fall back from.
const int kBaseWidth = 700;
const int kBaseHeight = 430;

// Two scale factors, as in the Arland launcher. g_dpiScale is the display's,
// applied because the process is DPI aware and lays itself out in real pixels.
// g_userScale is ours, for a TV or handheld where the DPI-correct size is still
// too small to read across a room.
int g_dpiScale = 100;
int g_userScale = 100;
int S(int value) { return value * g_dpiScale / 100 * g_userScale / 100; }

// ---- ini helpers -----------------------------------------------------------

bool iniString(const char* path, const char* section, const char* key,
               char* out, DWORD outSize, const char* def = "") {
  out[0] = '\0';
  if (!path[0])
    return false;
  GetPrivateProfileStringA(section, key, def, out, outSize, path);
  return out[0] != '\0';
}

bool iniBool(const char* path, const char* section, const char* key, bool def) {
  char value[16] = {};
  // \x01 is the "key absent" sentinel: an empty default cannot distinguish a
  // missing key from one someone deliberately blanked.
  if (!iniString(path, section, key, value, sizeof(value), "\x01") ||
      value[0] == '\x01')
    return def;
  return value[0] == 't' || value[0] == 'T' || value[0] == '1' ||
         value[0] == 'y' || value[0] == 'Y';
}

void iniWriteBool(const char* path, const char* section, const char* key,
                  bool on) {
  if (path[0])
    WritePrivateProfileStringA(section, key, on ? "true" : "false", path);
}

void iniWriteInt(const char* path, const char* section, const char* key,
                 unsigned value) {
  if (!path[0])
    return;
  char text[16];
  wsprintfA(text, "%u", value);
  WritePrivateProfileStringA(section, key, text, path);
}

void comboFill(HWND combo, const ComboItem* items, int count) {
  for (int i = 0; i < count; ++i)
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)items[i].label);
}

void comboSelectByValue(HWND combo, const ComboItem* items, int count,
                        const char* value, int fallback) {
  for (int i = 0; i < count; ++i) {
    if (!lstrcmpA(items[i].value, value)) {
      SendMessageW(combo, CB_SETCURSEL, i, 0);
      return;
    }
  }
  SendMessageW(combo, CB_SETCURSEL, fallback, 0);
}

const char* comboValue(HWND combo, const ComboItem* items, int count) {
  const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (index < 0 || index >= count)
    return items[0].value;
  return items[index].value;
}

// ---- resolution combo ------------------------------------------------------

// Every entry the resolution combo currently offers, in the order shown. Held
// alongside the control because the list is not a fixed table: the desktop's
// own mode and the ini's existing value are appended when not already present.
std::vector<Resolution> g_resolutions;

bool desktopResolution(unsigned* width, unsigned* height) {
  DEVMODEA mode = {};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmPelsWidth && mode.dmPelsHeight) {
    *width = mode.dmPelsWidth;
    *height = mode.dmPelsHeight;
    return true;
  }
  const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
  const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
  if (screenWidth <= 0 || screenHeight <= 0)
    return false;
  *width = unsigned(screenWidth);
  *height = unsigned(screenHeight);
  return true;
}

void addResolution(unsigned width, unsigned height) {
  for (const Resolution& r : g_resolutions) {
    if (r.width == width && r.height == height)
      return;
  }
  g_resolutions.push_back({ width, height });
}

// Rebuild the combo from g_resolutions, sorted by pixel count so an appended
// entry lands where it belongs rather than at the bottom.
// Rebuild the combo from g_resolutions. Auto stays pinned at index 0; the rest
// are sorted by pixel count so an appended entry lands where it belongs rather
// than at the bottom. `selectAuto` wins over the width/height pair.
void refillResolutions(unsigned selectWidth, unsigned selectHeight,
                       bool selectAuto) {
  // Everything but the Auto sentinel takes part in the sort.
  std::sort(g_resolutions.begin() + 1, g_resolutions.end(),
    [](const Resolution& a, const Resolution& b) {
      return (unsigned long long)a.width * a.height <
             (unsigned long long)b.width * b.height;
    });
  SendMessageW(g_hRes, CB_RESETCONTENT, 0, 0);
  unsigned desktopWidth = 0, desktopHeight = 0;
  const bool haveDesktop = desktopResolution(&desktopWidth, &desktopHeight);
  int selected = 0;
  for (size_t i = 0; i < g_resolutions.size(); ++i) {
    const Resolution& r = g_resolutions[i];
    wchar_t label[96];
    if (!r.width && !r.height) {
      // Auto carries what it currently resolves to, for the same reason the
      // Arland launcher shows it: the answer belongs in the list being chosen
      // from, not somewhere the user has to go and look it up.
      if (haveDesktop)
        wsprintfW(label, L"Auto  (%u x %u)", desktopWidth, desktopHeight);
      else
        lstrcpynW(label, L"Auto  (desktop resolution)", 96);
    } else if (haveDesktop && r.width == desktopWidth &&
               r.height == desktopHeight) {
      wsprintfW(label, L"%u x %u  (desktop)", r.width, r.height);
    } else {
      wsprintfW(label, L"%u x %u", r.width, r.height);
    }
    SendMessageW(g_hRes, CB_ADDSTRING, 0, (LPARAM)label);
    if (!selectAuto && r.width == selectWidth && r.height == selectHeight)
      selected = int(i);
  }
  SendMessageW(g_hRes, CB_SETCURSEL, selected, 0);
}

// ---- path and game resolution ----------------------------------------------

// Join `dir` (which must end in a separator) and `name` into a MAX_PATH `out`.
// False when the result would not fit, leaving out empty: lstrcatA takes no
// bound, so a folder near MAX_PATH would run past the caller's buffer.
bool joinPath(char* out, const char* dir, const char* name) {
  out[0] = '\0';
  const size_t dirLen = std::strlen(dir);
  const size_t nameLen = std::strlen(name);
  if (dirLen + nameLen + 1 > MAX_PATH)
    return false;
  std::memcpy(out, dir, dirLen);
  std::memcpy(out + dirLen, name, nameLen + 1);
  return true;
}

bool fileInDir(const char* dir, const char* name, char* out) {
  return joinPath(out, dir, name) &&
         GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

// Index into kGames of the game installed in `dir`, or -1. Either of a game's
// two executables is enough to recognise it.
int gameInFolder(const char* dir) {
  char candidate[MAX_PATH];
  for (int i = 0; i < kGameCount; ++i) {
    if (fileInDir(dir, kGames[i].english, candidate) ||
        fileInDir(dir, kGames[i].multilingual, candidate))
      return i;
  }
  return -1;
}

// The executable to start for a given [Lang] Language value.
//
// This is the mapping the stock launcher itself uses, read out of its own
// string-compare chain: "2" and anything unrecognized start the English build,
// "1", "3" and "4" the multilingual one. The two builds do not each carry every
// language, so this matters -- starting the English one with Language=3 gets
// English, which is exactly the bug this avoids. The comparison is against the
// whole value, as the launcher's is.
//
// If the build the language calls for is not installed, the other is used
// rather than refusing to start anything.
bool gameExeForLanguage(const char* language, char* out) {
  if (g_game < 0 || !g_gameDir[0])
    return false;
  const char* code = language ? language : "2";
  const bool english = lstrcmpA(code, "1") != 0 && lstrcmpA(code, "3") != 0 &&
                       lstrcmpA(code, "4") != 0;
  const Game& game = kGames[g_game];
  const char* first = english ? game.english : game.multilingual;
  const char* second = english ? game.multilingual : game.english;
  return fileInDir(g_gameDir, first, out) ||
         fileInDir(g_gameDir, second, out);
}

void adoptFolder(const char* dir, int game) {
  joinPath(g_iniPath, dir, "dusk-fix.ini");
  joinPath(g_settingsPath, dir, "Setting.ini");
  lstrcpynA(g_gameDir, dir, MAX_PATH);   // keeps its trailing separator
  g_game = game;
  if (game < 0)
    return;
  g_gameName = kGames[game].name;
  char language[16] = {};
  iniString(g_settingsPath, "Lang", "Language", language, sizeof(language), "2");
  if (!gameExeForLanguage(language, g_gameExePath))
    g_gameExePath[0] = '\0';
}

// Work out which game folder to configure.
//
// Normally that is the folder this executable sits in. The working directory is
// consulted only as a fallback, and it matters: Wine resolves a symlinked
// executable before reporting it, so a dusk-fix-launcher.exe symlinked into a
// game folder reports the link's TARGET as its own location and would otherwise
// configure the build directory it was linked from. The working directory stays
// the folder the tool was started in, which is the folder the user means --
// Explorer, the test scripts and the msimg32 redirect all set it that way.
bool resolveGameFolder() {
  char exeDir[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
  if (!n || n >= MAX_PATH)
    return false;
  char* slash = std::strrchr(exeDir, '\\');
  if (!slash)
    return false;
  slash[1] = '\0';

  int game = gameInFolder(exeDir);
  if (game >= 0) {
    adoptFolder(exeDir, game);
    return true;
  }

  char cwd[MAX_PATH] = {};
  const DWORD c = GetCurrentDirectoryA(MAX_PATH, cwd);
  if (c && c < MAX_PATH - 1) {
    if (cwd[c - 1] != '\\') {
      cwd[c] = '\\';
      cwd[c + 1] = '\0';
    }
    game = gameInFolder(cwd);
    if (game >= 0) {
      adoptFolder(cwd, game);
      return true;
    }
  }

  // No game either side: keep configuring the folder this executable is in,
  // which is where a user who put it somewhere odd would expect the ini.
  adoptFolder(exeDir, -1);
  return false;
}

// ---- load, save, dirty tracking --------------------------------------------

bool isChecked(HWND ctrl) {
  return ctrl && SendMessageW(ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setChecked(HWND ctrl, bool on) {
  if (ctrl)
    SendMessageW(ctrl, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Everything the window can change, packed so two states can be compared. Used
// only to decide whether closing needs to ask.
struct UiState {
  int  resolution;
  int  windowMode;
  int  language;
  bool outline;
  bool skipLauncher;

  bool operator == (const UiState& o) const {
    return resolution == o.resolution && windowMode == o.windowMode &&
           language == o.language &&
           outline == o.outline &&
           skipLauncher == o.skipLauncher;
  }
};

UiState currentState() {
  UiState state = {};
  state.resolution = int(SendMessageW(g_hRes, CB_GETCURSEL, 0, 0));
  state.windowMode = int(SendMessageW(g_hWinMode, CB_GETCURSEL, 0, 0));
  state.language = int(SendMessageW(g_hLang, CB_GETCURSEL, 0, 0));
  state.outline = isChecked(g_hOutline);
  state.skipLauncher = isChecked(g_hSkipLauncher);
  return state;
}

UiState g_savedState;
void markSaved() { g_savedState = currentState(); }
bool hasUnsavedChanges() { return !(currentState() == g_savedState); }

void loadFromIni() {
  // ---- the game's own Setting.ini
  //
  // Read with a 0 sentinel rather than the game's 1280x720 default, because
  // whether these keys exist at all is what decides the Auto default below.
  const unsigned width =
    GetPrivateProfileIntA("Graphics", "ScreenWidth", 0, g_settingsPath);
  const unsigned height =
    GetPrivateProfileIntA("Graphics", "ScreenHeight", 0, g_settingsPath);

  g_resolutions.assign(1, kAutoResolution);
  g_resolutions.insert(g_resolutions.end(), kResolutions,
    kResolutions + kResolutionCount);
  unsigned desktopWidth = 0, desktopHeight = 0;
  if (desktopResolution(&desktopWidth, &desktopHeight))
    addResolution(desktopWidth, desktopHeight);
  // Whatever the file already holds is offered too, so this window never
  // silently rewrites a resolution it did not have in its list.
  if (width && height)
    addResolution(width, height);
  // Auto is the launcher's own memory of a choice, not a value the game could
  // hold, so it lives in dusk-fix.ini rather than in Setting.ini.
  //
  // Its default is what a file that has never been saved gets, and it has to
  // be Auto to match the Arland launcher, which selects Auto whenever its own
  // ini carries no resolution (`int baseSel = 0; // Auto by default`). Dusk
  // defaulted to false, and since `AutoResolution` is never seeded at file
  // creation -- only SkipLauncher is -- a fresh install opened this window and
  // saw the game's shipped 1280x720 instead of its own display, which is
  // exactly the outcome resetToDefaults says defaults must not produce.
  //
  // The default is conditional rather than a flat `true`, though, because a
  // flat true would break the other rule this window keeps: opening it must
  // never silently replace a resolution the user already chose. If the game's
  // file already carries one, that choice wins and Auto is not assumed; a
  // deliberate 4K on a 1080p panel (downsampling) survives being looked at.
  // Auto is assumed only when there is no resolution to overrule.
  refillResolutions(width, height,
    iniBool(g_iniPath, "Launcher", "AutoResolution", !(width && height)));

  char value[16] = {};
  iniString(g_settingsPath, "Window", "FullScreen", value, sizeof(value), "0");
  comboSelectByValue(g_hWinMode, kWindowModeItems, kWindowModeCount, value, 0);

  iniString(g_settingsPath, "Lang", "Language", value, sizeof(value), "2");
  comboSelectByValue(g_hLang, kLangItems, kLangCount, value, 1);

  setChecked(g_hOutline,
    GetPrivateProfileIntA("Graphics", "Outline", 1, g_settingsPath) != 0);

  // ---- the mod's dusk-fix.ini
  //
  // Every shipping fix is on by default and absent from this window: the
  // font-atlas read cache, the two field-jitter halves, and the
  // high-resolution correction. None of them is a choice. The last is the one
  // that had to be argued rather than assumed -- rendering at 4K costs real
  // performance -- but choosing the resolution IS that decision, and the fix
  // only makes the resolution already chosen honest. A second checkbox asking
  // whether the chosen resolution should actually be used is not a preference.
  // Each has an environment switch for the A/B a bug report needs.
  //
  // What is left is the same on all three games, which is why nothing here is
  // gated on the engine any more.
  setChecked(g_hSkipLauncher,
    iniBool(g_iniPath, "Launcher", "SkipLauncher", false));

  markSaved();
}

void saveToIni() {
  const LRESULT index = SendMessageW(g_hRes, CB_GETCURSEL, 0, 0);
  if (index >= 0 && size_t(index) < g_resolutions.size()) {
    Resolution chosen = g_resolutions[size_t(index)];
    const bool automatic = !chosen.width && !chosen.height;
    // Auto is resolved to a literal here, because the game's own field can
    // only hold a number. If the display cannot be read, fall back to 1080p
    // rather than writing a zero the game would pass straight through: its
    // reader only replaces a NEGATIVE value with a default.
    if (automatic && !desktopResolution(&chosen.width, &chosen.height))
      chosen = { 1920, 1080 };
    iniWriteInt(g_settingsPath, "Graphics", "ScreenWidth", chosen.width);
    iniWriteInt(g_settingsPath, "Graphics", "ScreenHeight", chosen.height);
    iniWriteBool(g_iniPath, "Launcher", "AutoResolution", automatic);
  }
  WritePrivateProfileStringA("Window", "FullScreen",
    comboValue(g_hWinMode, kWindowModeItems, kWindowModeCount), g_settingsPath);
  WritePrivateProfileStringA("Lang", "Language",
    comboValue(g_hLang, kLangItems, kLangCount), g_settingsPath);
  WritePrivateProfileStringA("Graphics", "Outline",
    isChecked(g_hOutline) ? "1" : "0", g_settingsPath);

  iniWriteBool(g_iniPath, "Launcher", "SkipLauncher",
    isChecked(g_hSkipLauncher));
}

void resetToDefaults() {
  // Auto, not the game's own 1280x720 default: a fresh install looking far
  // worse than the screen it is running on is the outcome "defaults" should not
  // produce, and Auto keeps being right if the display changes later.
  unsigned width = 1920, height = 1080;
  if (desktopResolution(&width, &height))
    addResolution(width, height);
  refillResolutions(width, height, true);

  SendMessageW(g_hWinMode, CB_SETCURSEL, 0, 0);   // windowed
  SendMessageW(g_hLang, CB_SETCURSEL, 1, 0);      // English
  setChecked(g_hOutline, true);

  setChecked(g_hSkipLauncher, false);
}

// ---- starting things -------------------------------------------------------

bool stockToolPresent(const char* exeName) {
  if (!g_gameDir[0] || !exeName)
    return false;
  char path[MAX_PATH];
  return joinPath(path, g_gameDir, exeName) &&
         GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Start one of Koei Tecmo's own front-ends from the game folder.
//
// DUSK_NO_REDIRECT is set for the child: msimg32.dll sends the stock launcher
// here in the first place, so without it that button would only ever reopen
// this window. It is removed again immediately, so it never reaches the game
// when Play with mod is pressed afterwards.
bool runStockTool(const char* exeName) {
  char path[MAX_PATH];
  if (!g_gameDir[0] || !exeName || !joinPath(path, g_gameDir, exeName) ||
      GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    return false;

  SetEnvironmentVariableA("DUSK_NO_REDIRECT", "1");
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  const BOOL started = CreateProcessA(path, nullptr, nullptr, nullptr, FALSE,
    0, nullptr, g_gameDir, &startup, &process);
  SetEnvironmentVariableA("DUSK_NO_REDIRECT", nullptr);
  if (!started)
    return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

// Save, then start the game. `standDownMod` passes DUSK_DISABLE to the child,
// which makes d3d11.dll forward Direct3D and install nothing: the game as it
// shipped, from the same window, without moving files out of the folder and
// having to remember to move them back.
//
// Returns true when the game started, which is the caller's cue to close.
bool startGame(HWND w, bool standDownMod) {
  // Save first: starting the game with the settings still only on screen is the
  // one outcome nobody wants from either of these buttons. It matters just as
  // much without the mod, since resolution and language live in the game's own
  // settings file and it reads them either way.
  saveToIni();
  markSaved();   // nothing pending, so a failed launch leaves no close prompt

  // Which executable runs follows the language just saved, exactly as the
  // game's own launcher decides it. Read from the control rather than the file
  // so it is the selection in front of the user, not a stale one.
  char exePath[MAX_PATH] = {};
  const bool have =
    gameExeForLanguage(comboValue(g_hLang, kLangItems, kLangCount), exePath);

  if (standDownMod)
    SetEnvironmentVariableA("DUSK_DISABLE", "1");
  // CreateProcess rather than ShellExecute: the game has to be a child of this
  // process for Steam to keep counting the session as running, which is what
  // keeps the overlay and Steam Input attached to it.
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  const BOOL started = have && CreateProcessA(exePath, nullptr, nullptr,
    nullptr, FALSE, 0, nullptr, g_gameDir, &startup, &process);
  const DWORD error = GetLastError();
  // Removed immediately, so a later press of Play with mod in this same window
  // cannot inherit it and quietly launch without the mod.
  if (standDownMod)
    SetEnvironmentVariableA("DUSK_DISABLE", nullptr);

  if (!started) {
    wchar_t failed[320];
    wsprintfW(failed,
      L"The settings were saved, but %s could not be started (error %lu). "
      L"Launch the game as you normally would; the saved settings still apply.",
      g_gameName ? g_gameName : L"the game",
      have ? error : (DWORD)ERROR_FILE_NOT_FOUND);
    MessageBoxW(w, failed, L"Atelier Dusk Fixes", MB_OK | MB_ICONWARNING);
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

// The version of the mod installed beside this tool, read from d3d11.dll's own
// version resource. That file IS the mod, so asking it is more truthful than
// this launcher reporting its own version: the two ship together but nothing
// stops someone updating one and not the other, and when that happens the
// version worth knowing is the DLL's.
const wchar_t* modVersion() {
  static wchar_t version[64] = {};
  if (version[0])
    return version;
  lstrcpynW(version, L"not detected", 64);

  char dll[MAX_PATH] = {};
  if (!fileInDir(g_gameDir, "d3d11.dll", dll))
    return version;
  const DWORD size = GetFileVersionInfoSizeA(dll, nullptr);
  if (!size)
    return version;
  std::vector<BYTE> block(size);
  if (!GetFileVersionInfoA(dll, 0, size, block.data()))
    return version;
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!VerQueryValueW(block.data(), L"\\", (void**)&info, &infoSize) || !info)
    return version;
  // The build field is deliberately not shown: these are versioned major.minor
  // and a trailing 0.0 reads as noise.
  wsprintfW(version, L"%u.%u", HIWORD(info->dwFileVersionMS),
    LOWORD(info->dwFileVersionMS));
  return version;
}

// ---- window construction ---------------------------------------------------

// The font goes on at creation, not in a sweep afterwards: every height below
// is decided against it, so setting it later would mean laying the window out
// against the wrong one.
void setFont(HWND ctrl, HFONT font = nullptr) {
  if (ctrl)
    SendMessageW(ctrl, WM_SETFONT, (WPARAM)(font ? font : g_uiFont), TRUE);
}

// One line of UI text. Measured once, from the font the window actually draws
// in, and everything vertical is expressed against it.
void measureUiFont(HWND w) {
  g_lineHeight = S(16);   // only if the DC cannot be had
  HDC dc = GetDC(w);
  if (!dc)
    return;
  HFONT previous = (HFONT)SelectObject(dc, g_uiFont);
  TEXTMETRICW metrics = {};
  if (GetTextMetricsW(dc, &metrics) && metrics.tmHeight > 0)
    g_lineHeight = metrics.tmHeight;
  SelectObject(dc, previous);
  ReleaseDC(w, dc);
}

int labelHeight()   { return g_lineHeight; }
int controlHeight() { return g_lineHeight + S(10); }
int checkHeight()   { return std::max(g_lineHeight, S(16)); }
int buttonHeight()  { return g_lineHeight + S(12); }

// The mk* helpers create at a throwaway position; Layout moves everything into
// place afterwards. That is the Arland arrangement and it is what lets the
// layout be a single top-to-bottom pass rather than a set of coordinates that
// have to agree with each other.
HWND mkCheck(HWND parent, const wchar_t* text, int x, int y, int w, int id) {
  HWND c = CreateWindowExW(0, L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
    x, y, w, checkHeight(), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
  setFont(c);
  return c;
}

HWND mkCombo(HWND parent, int x, int y, int w, int id) {
  HWND c = CreateWindowExW(0, L"COMBOBOX", nullptr,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
    x, y, w, S(200), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
  setFont(c);
  return c;
}

HWND mkButton(HWND parent, const wchar_t* text, int x, int y, int w, int id) {
  HWND c = CreateWindowExW(0, L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    x, y, w, buttonHeight(), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
  setFont(c);
  return c;
}

// The font the window draws in: whatever the platform says its UI font is.
//
// SPI_GETNONCLIENTMETRICS gets Segoe UI on Windows 10 and 11 and whatever
// succeeds it later, and under Wine gets the prefix's own UI face. Asking the
// OS is the only thing that stays right across versions; DEFAULT_GUI_FONT is
// still the 1990s bitmap face. Nothing is bundled: the window should look like
// the desktop it is running on, and that is as true of a Proton prefix as of
// Windows.
HFONT createUiFont() {
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
      &metrics, 0)) {
    LOGFONTW font = metrics.lfMessageFont;
    // g_userScale only: this height already carries the display's DPI, since
    // the process is DPI aware.
    font.lfHeight = font.lfHeight * g_userScale / 100;
    // ClearType explicitly rather than DEFAULT_QUALITY, which under Wine can
    // resolve to unsmoothed rendering and is what makes small text look ragged
    // there even with a good face.
    font.lfQuality = CLEARTYPE_QUALITY;
    if (HFONT created = CreateFontIndirectW(&font))
      return created;
  }
  return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

// The same font, bold, for the section headings. Without it a heading is the
// same weight, size and colour as the label under it, so the sections it is
// supposed to divide read as one undifferentiated column.
HFONT createHeadingFont() {
  LOGFONTW font = {};
  if (g_uiFont && GetObjectW(g_uiFont, sizeof(font), &font)) {
    font.lfWeight = FW_SEMIBOLD;
    if (HFONT created = CreateFontIndirectW(&font))
      return created;
  }
  return g_uiFont;
}

// The DPI the window will be laid out at. GetDpiForSystem is Windows 10 and
// later; the desktop DC gives the same answer everywhere else, Wine included.
int systemDpi() {
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    using PFN_GetDpiForSystem = UINT (WINAPI*)();
    if (auto getDpi =
          (PFN_GetDpiForSystem)GetProcAddress(user32, "GetDpiForSystem"))
      return (int)getDpi();
  }
  HDC screen = GetDC(nullptr);
  const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
  if (screen)
    ReleaseDC(nullptr, screen);
  return dpi > 0 ? dpi : 96;
}

// The work area of the monitor the window will open on (the one holding the
// cursor), which is what the layout has to fit inside.
bool cursorWorkArea(RECT* area) {
  POINT cursor = {};
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  if (!GetCursorPos(&cursor) ||
      !GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY),
                       &monitor))
    return false;
  *area = monitor.rcWork;
  return true;
}

// Decide both scale factors, then reduce the enlargement until the window fits
// the screen it is opening on. The base layout is sized for 720p, so this can
// always fall back to no enlargement at all and still fit.
//
// DUSK_UI_SCALE sets the enlargement directly, as a percentage. It is how the
// large layout gets tested without a TV, and how someone on a handheld can ask
// for it.
void chooseScale(DWORD windowStyle) {
  g_dpiScale = systemDpi() * 100 / 96;
  if (g_dpiScale < 100)
    g_dpiScale = 100;

  g_userScale = 100;
  if (const char* requested = std::getenv("DUSK_UI_SCALE")) {
    const int value = std::atoi(requested);
    if (value >= 100 && value <= 200)
      g_userScale = value;
  }

  RECT area = {};
  if (!cursorWorkArea(&area))
    return;
  const int availableWidth = area.right - area.left;
  const int availableHeight = area.bottom - area.top;
  while (g_userScale > 100) {
    RECT window = { 0, 0, S(kBaseWidth), S(kBaseHeight) };
    AdjustWindowRect(&window, windowStyle, FALSE);
    if (window.right - window.left <= availableWidth &&
        window.bottom - window.top <= availableHeight)
      break;
    g_userScale -= 5;
  }
}

// Whether this is running under Wine, which on a Linux desktop or a Steam Deck
// means Proton. The canonical test: Wine's ntdll exports a version function no
// Windows one has.
bool runningUnderWine() {
  static const bool wine = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }();
  return wine;
}

// Whether to defer to the platform's own look. True on Windows, where the
// visual style is worth having and fighting it is what went wrong; false under
// Wine, where the defaults are the reason the overrides exist.
bool nativeStyling() { return !runningUnderWine(); }

// The colour a themed tab control paints its page in.
//
// Asked for rather than assumed, because it is a property of whichever visual
// style the user is running and has never been reliably white. The theme
// usually defines the body as a bitmap rather than a flat colour, so
// GetThemeColor is not dependable here; drawing the part into a scratch bitmap
// and reading the middle pixel gets the real answer for a flat fill and the
// dominant one for a gradient, which is what we want to match controls to.
COLORREF themedTabBodyColor() {
  HTHEME theme = OpenThemeData(nullptr, L"TAB");
  if (!theme)
    return GetSysColor(COLOR_BTNFACE);

  COLORREF sampled = CLR_INVALID;
  if (HDC screen = GetDC(nullptr)) {
    if (HDC scratch = CreateCompatibleDC(screen)) {
      if (HBITMAP surface = CreateCompatibleBitmap(screen, 64, 64)) {
        HGDIOBJ previous = SelectObject(scratch, surface);
        RECT area = { 0, 0, 64, 64 };
        // Primed with the dialog face first: a fresh bitmap holds whatever was
        // in that memory, and a theme part that is partly transparent would
        // otherwise leave us sampling it.
        if (HBRUSH prime = CreateSolidBrush(GetSysColor(COLOR_BTNFACE))) {
          FillRect(scratch, &area, prime);
          DeleteObject(prime);
        }
        if (SUCCEEDED(DrawThemeBackground(theme, scratch, TABP_BODY, 0, &area,
                                          nullptr)))
          sampled = GetPixel(scratch, 32, 32);
        SelectObject(scratch, previous);
        DeleteObject(surface);
      }
      DeleteDC(scratch);
    }
    ReleaseDC(nullptr, screen);
  }
  CloseThemeData(theme);
  return sampled == CLR_INVALID ? GetSysColor(COLOR_WINDOW) : sampled;
}

// Decide the palette once, before any brush or control exists.
void initStyling() {
  if (!nativeStyling())
    return;   // the white-everywhere defaults above are the Wine regime
  g_windowBack = GetSysColor(COLOR_BTNFACE);
  g_pageBack = themedTabBodyColor();
  // Taken from the system too, so a dark or high-contrast scheme stays legible
  // instead of being black text on a background chosen for a light one.
  g_text = GetSysColor(COLOR_WINDOWTEXT);
  g_secondaryText = GetSysColor(COLOR_GRAYTEXT);
}

// Wear the icon of whichever game this folder holds. The window is about that
// game, and on a taskbar with several open it is the only thing telling them
// apart. Nothing to clean up: the icons live as long as the process.
void applyGameIcon(HWND w) {
  if (!g_gameExePath[0])
    return;
  // Not named "small": the Windows headers define that as a macro for char,
  // which MinGW tolerates here and MSVC does not.
  HICON largeIcon = nullptr;
  HICON smallIcon = nullptr;
  ExtractIconExA(g_gameExePath, 0, &largeIcon, &smallIcon, 1);
  if (largeIcon)
    SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)largeIcon);
  if (smallIcon)
    SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
}

void onPage(int page, HWND ctrl) {
  if (ctrl && page >= 0 && page < kPageCount && g_pageCount[page] < 40)
    g_pageCtrls[page][g_pageCount[page]++] = ctrl;
}

// Repaint the whole area a control covers, background included. The labels are
// drawn without a background of their own (see WM_CTLCOLORSTATIC), so whatever
// they had before stays on screen until the tab page underneath is redrawn --
// which needs the parent, the tab control and the label itself, in that order.
void repaintUnder(HWND ctrl) {
  HWND parent = GetParent(ctrl);
  if (!parent)
    return;
  RECT area;
  GetWindowRect(ctrl, &area);
  MapWindowPoints(nullptr, parent, (POINT*)&area, 2);
  RedrawWindow(parent, &area, nullptr,
    RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void showPage(int page) {
  for (int p = 0; p < kPageCount; ++p)
    for (int i = 0; i < g_pageCount[p]; ++i)
      ShowWindow(g_pageCtrls[p][i], p == page ? SW_SHOW : SW_HIDE);
  // The outgoing page's labels leave their text behind them, so the page is
  // redrawn as a whole rather than relying on each control to clean up after
  // itself.
  if (g_hTabs)
    repaintUnder(g_hTabs);
}

// ---- layout ----------------------------------------------------------------
//
// Controls are stacked by a cursor instead of being placed at hand-picked
// coordinates, and every note's height is MEASURED at the width it will be
// drawn at.
//
// That measurement is the point of this. A static silently drops any line past
// its height, so a note given a fixed two lines turns a third line into a
// sentence ending mid-word -- with nothing in the build, the log or the code to
// say so. It shipped that way twice in the Arland project. Measuring makes the
// row as tall as its text, which also means notes can be reworded and settings
// reordered without recomputing anything below them.
//
// Everything in here is DEVICE pixels. The logical constants go through S() on
// the way in, so nothing downstream has to track which of the two it is
// holding -- the other half of the same class of bug.
struct Layout {
  HWND parent;
  int page;
  int y;

  // Starts inside the tab control's real display area, wherever the theme and
  // the font put it, rather than at a guessed header height.
  Layout(HWND parentWindow, int tabPage)
    : parent(parentWindow), page(tabPage), y(g_pageRect.top + S(10)) {}

  // Every page reports how far it got, so the window can be sized to the
  // tallest of them.
  ~Layout() { g_contentBottom = std::max(g_contentBottom, y); }

  // Columns, measured from the page's own edges. Checkbox rows use a nearer
  // note column: a checkbox carries its own label, so leaving its note out at
  // the combo note column strands it across a gap of empty space.
  static int left()          { return g_pageRect.left + S(8); }
  static int right()         { return g_pageRect.right - S(8); }
  static int labelWidth()    { return S(150); }
  static int controlLeft()   { return left() + S(156); }
  static int controlWidth()  { return S(230); }
  static int noteLeft()      { return left() + S(406); }
  static int noteWidth()     { return right() - noteLeft(); }
  static int checkNoteLeft() { return left() + S(276); }
  static int checkNoteWidth(){ return right() - checkNoteLeft(); }
  static int fullWidth()     { return right() - left(); }

  int measure(const wchar_t* text, int width) const {
    HDC dc = GetDC(parent);
    if (!dc)
      return S(32);
    HFONT previous = (HFONT)SelectObject(dc, g_uiFont);
    RECT box = { 0, 0, width, 0 };
    DrawTextW(dc, text, -1, &box, DT_CALCRECT | DT_WORDBREAK);
    SelectObject(dc, previous);
    ReleaseDC(parent, dc);
    return box.bottom;
  }

  HWND place(const wchar_t* cls, const wchar_t* text, DWORD style,
             int x, int top, int width, int height, HFONT font = nullptr) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
      x, top, width, height, parent, nullptr, nullptr, nullptr);
    setFont(control, font);
    onPage(page, control);
    return control;
  }

  // A note, at its measured height, remembered so WM_CTLCOLORSTATIC draws it in
  // the secondary colour. Returns the height it took.
  int note(const wchar_t* text, int x, int width, int top) {
    const int height = measure(text, width);
    HWND label = place(L"STATIC", text, 0, x, top, width, height);
    if (g_descCount < 32)
      g_hDesc[g_descCount++] = label;
    return height;
  }

  // label + control + note. The row is as tall as whichever side needs more.
  void row(const wchar_t* labelText, HWND control, const wchar_t* noteText) {
    // The label is centred against the control rather than nudged down by a
    // fixed four pixels, which only looked centred at one font size.
    place(L"STATIC", labelText, 0, left(),
      y + (controlHeight() - labelHeight()) / 2, labelWidth(), labelHeight());
    // Combos are moved with their dropdown extent, not their visible height:
    // for a combo box the height passed here is how far the list drops.
    MoveWindow(control, controlLeft(), y, controlWidth(), S(200), TRUE);
    onPage(page, control);
    int used = controlHeight();
    if (noteText)
      used = std::max(used, note(noteText, noteLeft(), noteWidth(), y));
    y += used + S(12);
  }

  void checkRow(HWND check, const wchar_t* noteText) {
    MoveWindow(check, left(), y, checkNoteLeft() - left() - S(16),
      checkHeight(), TRUE);
    onPage(page, check);
    int used = checkHeight();
    if (noteText)
      used = std::max(used, note(noteText, checkNoteLeft(), checkNoteWidth(), y));
    y += used + S(12);
  }

  // Bold, and with more air above it than below, so it binds to the rows it
  // introduces rather than floating between two groups.
  void heading(const wchar_t* text) {
    y += S(10);
    place(L"STATIC", text, 0, left(), y, fullWidth(), labelHeight(),
      g_headingFont);
    y += labelHeight() + S(8);
  }

  // Full-width text in the primary colour: a statement of fact rather than an
  // explanation of a control, so it does not get the notes' grey.
  void label(const wchar_t* text) {
    const int height = measure(text, fullWidth());
    place(L"STATIC", text, 0, left(), y, fullWidth(), height);
    y += height + S(8);
  }

  // A note that belongs to the page rather than to one control.
  void fullNote(const wchar_t* text) {
    y += note(text, left(), fullWidth(), y) + S(12);
  }

  void buttons(HWND a, HWND b, HWND c) {
    const int gap = S(12);
    // Divided out of the page width rather than fixed, so three buttons always
    // span the same column as everything else however wide the text makes them.
    const int width = (fullWidth() - 2 * gap) / 3;
    const int height = buttonHeight();
    MoveWindow(a, left(), y, width, height, TRUE);
    MoveWindow(b, left() + width + gap, y, width, height, TRUE);
    MoveWindow(c, left() + 2 * (width + gap), y, width, height, TRUE);
    onPage(page, a); onPage(page, b); onPage(page, c);
    y += height + S(12);
  }

  // A SysLink measures itself: LM_GETIDEALSIZE takes the width it will be given
  // and reports the height that width needs. Asked rather than assumed, for the
  // same reason the notes are measured -- and with a fallback, since the message
  // needs ComCtl32 v6 and Wine does not necessarily answer it.
  void link(HWND control) {
    int height = labelHeight() + S(4);
    SIZE ideal = {};
    if (SendMessageW(control, LM_GETIDEALSIZE, (WPARAM)fullWidth(),
                     (LPARAM)&ideal) && ideal.cy > 0)
      height = std::max(height, (int)ideal.cy);
    MoveWindow(control, left(), y, fullWidth(), height, TRUE);
    onPage(page, control);
    y += height + S(12);
  }
};

void createControls(HWND w) {
  // Before anything is placed: the font decides every height below it.
  measureUiFont(w);

  // The frame is derived from the client area and the button height rather than
  // from the literal window size, so the bottom row sits a fixed margin off the
  // bottom edge at any font size.
  RECT client = {};
  GetClientRect(w, &client);
  const int margin = S(12);
  int buttonTop = client.bottom - S(14) - buttonHeight();

  g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP, margin, margin,
    client.right - 2 * margin, buttonTop - 2 * margin,
    w, (HMENU)(INT_PTR)IDC_TABS, nullptr, nullptr);
  // Set before the items go in and before the page rect is taken: the header's
  // height comes from this font, and everything on the pages is positioned
  // against that height.
  setFont(g_hTabs);
  SetWindowSubclass(g_hTabs, TabProc, 0, 0);
  TCITEMW tab = {};
  tab.mask = TCIF_TEXT;
  const wchar_t* pageNames[kPageCount] = { L"Display", L"Game", L"About" };
  for (int i = 0; i < kPageCount; ++i) {
    tab.pszText = (LPWSTR)pageNames[i];
    SendMessageW(g_hTabs, TCM_INSERTITEMW, i, (LPARAM)&tab);
  }

  // Where the pages may actually draw. TCM_ADJUSTRECT is the only thing that
  // knows how tall this theme's header turned out with this font; mapping the
  // result into the parent's coordinates is what lets the page contents stay
  // children of the main window while being positioned against the tab.
  GetClientRect(g_hTabs, &g_pageRect);
  SendMessageW(g_hTabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&g_pageRect);
  MapWindowPoints(g_hTabs, w, (POINT*)&g_pageRect, 2);

  // Which game this folder is: the tool configures whatever it sits next to, so
  // this is the one fact worth stating outright. It sits on the tab strip's own
  // row, right-aligned, where it reads as a heading for the window rather than
  // competing with the tabs. Created AFTER the tab control so it is above it in
  // z-order, and painted transparently (see WM_CTLCOLORSTATIC) so the strip
  // shows through instead of a white block sitting on it.
  //
  // Positioned from the strip's own item rectangle rather than from a fixed y:
  // the strip is as tall as the theme and the font make it, and a constant that
  // sat neatly on it under Wine sat across its lower border on Windows.
  RECT strip = {};
  SendMessageW(g_hTabs, TCM_GETITEMRECT, 0, (LPARAM)&strip);
  MapWindowPoints(g_hTabs, w, (POINT*)&strip, 2);
  const int labelTop =
    strip.top + ((strip.bottom - strip.top) - labelHeight()) / 2;
  const int labelRight = g_pageRect.right - S(4);
  const int labelLeft =
    std::max((int)strip.right + S(12), labelRight - S(320));
  g_hGameLabel = CreateWindowExW(0, L"STATIC",
    g_gameName ? g_gameName : L"No game detected",
    WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_ENDELLIPSIS,
    labelLeft, labelTop, labelRight - labelLeft, labelHeight(),
    w, nullptr, nullptr, nullptr);
  setFont(g_hGameLabel);

  // ---------------- page 0: Display ----------------
  {
    Layout page(w, 0);
    g_hRes = mkCombo(w, 0, 0, 10, IDC_RES);
    page.row(L"Resolution:", g_hRes,
      L"Written to the game's own settings, which is where it lives. Not "
      L"filtered through Windows' display modes, so a mode your screen can "
      L"use is never missing.");

    g_hWinMode = mkCombo(w, 0, 0, 10, IDC_WINMODE);
    comboFill(g_hWinMode, kWindowModeItems, kWindowModeCount);
    page.row(L"Window mode:", g_hWinMode,
      L"Fullscreen takes over the display; windowed is friendlier to "
      L"alt-tab and to compositors.");

    // The stock front-ends are still reachable: this tool replaces them, it
    // does not remove them. Greyed out when the executable is not present.
    page.heading(L"The game as it shipped");
    g_hOpenEnv = mkButton(w, L"Settings &editor", 0, 0, 10, IDC_OPENENV);
    g_hOpenLauncher = mkButton(w, L"&Original launcher", 0, 0, 10,
      IDC_OPENLAUNCHER);
    g_hPlayVanilla = mkButton(w, L"Play &without the mod", 0, 0, 10,
      IDC_PLAYVANILLA);
    page.buttons(g_hOpenEnv, g_hOpenLauncher, g_hPlayVanilla);
    if (!stockToolPresent(g_game >= 0 ? kGames[g_game].stockEnv : nullptr))
      EnableWindow(g_hOpenEnv, FALSE);
    if (!stockToolPresent(g_game >= 0 ? kGames[g_game].stockLauncher : nullptr))
      EnableWindow(g_hOpenLauncher, FALSE);
    if (!g_gameExePath[0])
      EnableWindow(g_hPlayVanilla, FALSE);
    page.fullNote(
      L"Koei Tecmo's own settings editor and launcher, unmodified. The third "
      L"saves and starts the game with the mod stood down, changing nothing.");
  }

  // ---------------- page 1: Game ----------------
  {
    Layout page(w, 1);
    g_hLang = mkCombo(w, 0, 0, 10, IDC_LANG);
    comboFill(g_hLang, kLangItems, kLangCount);
    page.row(L"Language:", g_hLang,
      L"Written to the game's own settings, and decides which of the game's "
      L"two executables starts.");

    g_hOutline = mkCheck(w, L"Character outlines", 0, 0, 10, IDC_OUTLINE);
    page.checkRow(g_hOutline,
      L"The game's own outline rendering. On as it shipped.");
  }

  // ---------------- page 2: About ----------------
  // What is installed, where it came from, and what it is not.
  {
    Layout page(w, 2);
    wchar_t installed[160];
    wsprintfW(installed, L"Mod version: %s", modVersion());
    page.label(installed);
    page.fullNote(
      L"Read from d3d11.dll in this folder, which is the mod itself. "
      L"“Not detected” there means the game is running unmodified.");
    page.fullNote(
      L"Free and open source, and not affiliated with or endorsed by Koei "
      L"Tecmo or Gust. It is an unofficial attempt to fix bugs in the "
      L"original games, and it is never sold.");

    // A SysLink rather than a static: it is focusable, so it can be reached
    // with the keyboard or a controller, and it draws in the system's link
    // colour instead of an imitation of one.
    wchar_t markup[320];
    wsprintfW(markup, L"<a href=\"%s\">%s</a>", kRepositoryUrl, kRepositoryUrl);
    g_hRepoLink = CreateWindowExW(0, WC_LINK, markup,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, w,
      (HMENU)(INT_PTR)IDC_REPOLINK, nullptr, nullptr);
    // A SysLink does not inherit the parent's font, and it is the one control
    // here not built by a helper that sets it, so it has to be told explicitly.
    setFont(g_hRepoLink);
    page.link(g_hRepoLink);
  }

  // Grow the window if the tallest page outran the space set aside for it.
  //
  // The alternative is what a fixed layout does: clip the last row, with
  // nothing on screen to say a control is missing. Clamped to the monitor's
  // work area, so this can never push the button row off the bottom of the
  // screen -- if the text is too large to fit even a full-height window, the
  // clipping comes back, but only in the case where nothing else would fit
  // either.
  if (g_contentBottom + S(10) > g_pageRect.bottom) {
    int grow = g_contentBottom + S(10) - g_pageRect.bottom;
    RECT frame = { 0, 0, client.right, client.bottom + grow };
    AdjustWindowRect(&frame, (DWORD)GetWindowLongPtrW(w, GWL_STYLE), FALSE);
    int outerHeight = frame.bottom - frame.top;

    RECT work = {};
    const bool haveWork = cursorWorkArea(&work);
    if (haveWork && outerHeight > work.bottom - work.top) {
      grow -= outerHeight - (work.bottom - work.top);
      outerHeight = work.bottom - work.top;
    }
    if (grow > 0) {
      RECT current = {};
      GetWindowRect(w, &current);
      // Grown symmetrically, so a window that was centred stays centred.
      int top = current.top - grow / 2;
      if (haveWork) {
        if (top + outerHeight > work.bottom)
          top = work.bottom - outerHeight;
        if (top < work.top)
          top = work.top;
      }
      SetWindowPos(w, nullptr, current.left, top,
        current.right - current.left, outerHeight,
        SWP_NOZORDER | SWP_NOACTIVATE);

      client.bottom += grow;
      buttonTop += grow;
      g_pageRect.bottom += grow;
      MoveWindow(g_hTabs, margin, margin, client.right - 2 * margin,
        buttonTop - 2 * margin, TRUE);
    }
  }

  // The bottom row, placed against the measured button height and the window's
  // own edges rather than at literal coordinates, so it stays on the same
  // baseline as the tab control above it whatever the font does.
  //
  // Play with mod is bottom left, away from Close so it cannot be hit by
  // accident. It is also the default button and takes focus at startup: most of
  // the time this window is opened on the way into the game, not to change
  // something, so Enter should start playing. That matters most on a controller
  // or a handheld, where the alternative is driving a cursor across the window.
  const int buttonH = buttonHeight();
  const int closeW = S(90);
  const int wideW = S(150);
  const int rightEdge = client.right - margin;

  g_hStart = CreateWindowExW(0, L"BUTTON", L"&Play with mod",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
    margin, buttonTop, wideW, buttonH, w, (HMENU)(INT_PTR)IDC_START,
    nullptr, nullptr);
  setFont(g_hStart);
  if (!g_gameExePath[0])
    EnableWindow(g_hStart, FALSE);

  // Distinct mnemonics across the whole window: P play, R reset, C close,
  // E editor, O original launcher, W play without the mod, S skip.
  HWND reset = CreateWindowExW(0, L"BUTTON", L"&Reset to defaults",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    rightEdge - closeW - S(12) - wideW, buttonTop, wideW, buttonH, w,
    (HMENU)(INT_PTR)IDC_RESET, nullptr, nullptr);
  setFont(reset);

  // [Launcher] SkipLauncher, beside Play with mod because that is what it is
  // about: the next launch from Steam does what that button does, without
  // stopping here first. It belongs to no tab page, so it is not registered
  // with onPage and stays visible whichever page is showing -- which also means
  // it stands on the window background rather than a tab page, and
  // WM_CTLCOLORBTN colours it accordingly.
  //
  // Two lines, because the qualifier is the half that is easy to get wrong:
  // this is about the launch Steam performs, not about the window in front of
  // you. BS_MULTILINE is what makes the break in the caption take effect.
  const int skipLeft = margin + wideW + S(16);
  const int skipHeight = 2 * checkHeight();
  g_hSkipLauncher = CreateWindowExW(0, L"BUTTON",
    L"&Skip the launcher\n(when launching via Steam)",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE,
    skipLeft, buttonTop + (buttonH - skipHeight) / 2,
    std::max(S(60), (rightEdge - closeW - S(12) - wideW) - S(12) - skipLeft),
    skipHeight, w, (HMENU)(INT_PTR)IDC_SKIPLAUNCHER, nullptr, nullptr);
  setFont(g_hSkipLauncher);

  HWND close = CreateWindowExW(0, L"BUTTON", L"&Close",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    rightEdge - closeW, buttonTop, closeW, buttonH, w,
    (HMENU)(INT_PTR)IDC_CLOSE, nullptr, nullptr);
  setFont(close);

  showPage(0);
}

// Under Wine, paint the tab page in the one flat colour that regime uses.
//
// On Windows this does nothing, deliberately. A themed tab control draws its
// body during WM_PAINT, not WM_ERASEBKGND, so a fill here is drawn and then
// immediately painted over -- which is why the page stayed the theme's colour
// however white the brush was, while the controls on it obeyed the brush and
// became white patches. On that platform the page is the theme's to paint, and
// the controls are matched to it instead (see initStyling and WM_CTLCOLOR).
LRESULT CALLBACK TabProc(HWND tabs, UINT msg, WPARAM wp, LPARAM lp,
                         UINT_PTR, DWORD_PTR) {
  if (msg == WM_ERASEBKGND && !nativeStyling()) {
    const LRESULT handled = DefSubclassProc(tabs, msg, wp, lp);
    RECT page = {};
    GetClientRect(tabs, &page);
    SendMessageW(tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    FillRect((HDC)wp, &page, g_pageBrush);
    return handled;
  }
  if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(tabs, TabProc, 0);
  return DefSubclassProc(tabs, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      createControls(w);
      loadFromIni();
      applyGameIcon(w);
      SetFocus(g_hStart);
      return 0;

    case WM_NOTIFY: {
      const NMHDR* header = (const NMHDR*)lp;
      if (header && header->hwndFrom == g_hRepoLink &&
          (header->code == NM_CLICK || header->code == NM_RETURN)) {
        ShellExecuteW(w, L"open", kRepositoryUrl, nullptr, nullptr,
          SW_SHOWNORMAL);
        return 0;
      }
      if (header && header->hwndFrom == g_hTabs &&
          header->code == TCN_SELCHANGE) {
        showPage((int)SendMessageW(g_hTabs, TCM_GETCURSEL, 0, 0));
        return 0;
      }
      break;
    }

    // Checkboxes send WM_CTLCOLORBTN rather than WM_CTLCOLORSTATIC, and a
    // themed one ignores a hollow brush and fills with the dialog face anyway,
    // so both messages are answered the same way: with the colour the surface
    // the control actually stands on is painted in.
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
      // The game name lies on top of the tab control's own header strip, which
      // is not the window background, so it takes no background of its own.
      if ((HWND)lp == g_hGameLabel) {
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, g_secondaryText);
        return (LRESULT)GetStockObject(NULL_BRUSH);
      }
      // The skip checkbox is the one control that sits in the button row rather
      // than on a page, so it takes the window's own background. With the page
      // colour it would read as a patch laid on the strip, which is exactly
      // what the rule below exists to prevent everywhere else.
      if ((HWND)lp == g_hSkipLauncher) {
        SetBkColor((HDC)wp, g_windowBack);
        SetTextColor((HDC)wp, g_text);
        return (LRESULT)g_windowBrush;
      }
      // Every other static and checkbox stands on the tab page, so all of them
      // get the page's colour -- the theme's on Windows, the flat white under
      // Wine. This is what stops them reading as patches laid over the panel.
      SetBkColor((HDC)wp, g_pageBack);
      SetTextColor((HDC)wp, g_text);
      // The notes beside each control are secondary text, so they are drawn
      // grey rather than competing with the labels they explain.
      for (int i = 0; i < g_descCount; ++i) {
        if (g_hDesc[i] == (HWND)lp) {
          SetTextColor((HDC)wp, g_secondaryText);
          break;
        }
      }
      return (LRESULT)g_pageBrush;
    }

    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_START:
          if (startGame(w, false))
            DestroyWindow(w);
          return 0;
        case IDC_PLAYVANILLA:
          if (startGame(w, true))
            DestroyWindow(w);
          return 0;
        case IDC_OPENLAUNCHER:
        case IDC_OPENENV: {
          // Saved first, so the stock tool opens onto the settings on screen
          // rather than the ones on disk. Both of them edit the same
          // Setting.ini this window does.
          saveToIni();
          markSaved();
          const char* tool = g_game < 0 ? nullptr
            : (LOWORD(wp) == IDC_OPENLAUNCHER ? kGames[g_game].stockLauncher
                                              : kGames[g_game].stockEnv);
          if (!runStockTool(tool))
            MessageBoxW(w, L"That program could not be started.",
              L"Atelier Dusk Fixes", MB_OK | MB_ICONWARNING);
          return 0;
        }
        case IDC_RESET:
          resetToDefaults();
          return 0;
        case IDC_CLOSE:
          SendMessageW(w, WM_CLOSE, 0, 0);
          return 0;
        default:
          break;
      }
      break;

    case WM_CLOSE:
      if (hasUnsavedChanges()) {
        const int answer = MessageBoxW(w,
          L"Save the changed settings before closing?",
          L"Atelier Dusk Fixes", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL)
          return 0;
        if (answer == IDYES) {
          saveToIni();
          markSaved();
        }
      }
      DestroyWindow(w);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
  INITCOMMONCONTROLSEX controls = {};
  controls.dwSize = sizeof(controls);
  // LINK_CLASS registers SysLink, TAB_CLASSES the tab control. Without the v6
  // manifest this quietly gets the 5.82 set.
  controls.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_LINK_CLASS;
  InitCommonControlsEx(&controls);

  resolveGameFolder();
  initStyling();

  const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX);
  chooseScale(style);
  g_uiFont = createUiFont();
  g_headingFont = createHeadingFont();
  g_windowBrush = CreateSolidBrush(g_windowBack);
  g_pageBrush = CreateSolidBrush(g_pageBack);

  WNDCLASSEXW cls = {};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = WndProc;
  cls.hInstance = instance;
  // IDC_ARROW is an integer atom the headers hand back as LPSTR unless UNICODE
  // is defined; this file calls the W entry points explicitly instead, so the
  // atom is cast rather than the whole translation unit switched over.
  cls.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  cls.hbrBackground = g_windowBrush;
  cls.lpszClassName = L"DuskFixLauncher";
  if (!RegisterClassExW(&cls))
    return 1;

  RECT frame = { 0, 0, S(kBaseWidth), S(kBaseHeight) };
  AdjustWindowRect(&frame, style, FALSE);
  const int frameWidth = frame.right - frame.left;
  const int frameHeight = frame.bottom - frame.top;
  RECT area = {};
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  if (cursorWorkArea(&area)) {
    x = area.left + (area.right - area.left - frameWidth) / 2;
    y = area.top + (area.bottom - area.top - frameHeight) / 2;
  }

  HWND window = CreateWindowExW(0, cls.lpszClassName, L"Atelier Dusk Fixes",
    style, x, y, frameWidth, frameHeight, nullptr, nullptr, instance, nullptr);
  if (!window)
    return 1;

  if (g_game < 0)
    MessageBoxW(window,
      L"No Atelier Dusk game was found in this folder, so there is nothing to "
      L"start. Put dusk-fix-launcher.exe next to the game executable.",
      L"Atelier Dusk Fixes", MB_OK | MB_ICONINFORMATION);

  ShowWindow(window, show);
  UpdateWindow(window);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    // IsDialogMessage gives a plain window the tab navigation, Escape and
    // default-button Enter behaviour expected of a settings dialog.
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return 0;
}
