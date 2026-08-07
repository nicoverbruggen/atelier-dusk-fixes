// SPDX-License-Identifier: MIT
//
// dusk-fix-launcher.exe: the mod's launcher. It edits the settings the mod has
// and the game's own, and starts the game; it is what msimg32.dll opens in
// place of Koei Tecmo's launcher when the game is started from Steam.
//
// It is the Arland project's src/config_gui/main.cpp, page for page and row for
// row: the same three tabs, the same order within them, the same wording on the
// notes, the same bottom button row, the same save-failure reporting. Someone
// who has used one should not have to learn the other. Where Arland has a
// control for a feature this mod does not have -- shadow detail, the UI font,
// attack cut-ins, the developer views, verbose logging -- the row is simply
// absent rather than present and inert.
//
// Two differences are deliberate and are argued where they occur: how "Auto"
// resolution is resolved (see loadFromIni), and what Reset returns the
// resolution to (see resetToDefaults). Both follow from this mod having no
// resolution override of its own.
//
// Two files are edited, and the split matters:
//
//   Setting.ini   the game's own, which it reads by itself. Resolution lives
//                 here and nowhere else -- Ayesha accepts any value in it and
//                 validates nothing but a negative number, so the mod
//                 deliberately has no resolution setting of its own to
//                 duplicate it with.
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
  IDC_SSAA,
  IDC_RENDLBL,        // read-only note under the supersampling row
  IDC_SMAA,
  IDC_OUTLINE,
  IDC_SKIPLOGOS,      // skip the boot logos
  IDC_SKIPMOVIE,      // skip the movies before the title screen
  IDC_SKIPLAUNCHER,
  IDC_START,
  IDC_OPENLAUNCHER,   // Koei Tecmo's own launcher
  IDC_OPENENV,        // Koei Tecmo's own settings editor
  IDC_PLAYVANILLA,    // the game with the mod turned off
  IDC_RESET,
  IDC_CLOSE,
  IDC_REPOLINK,
};

// Shown in full and opened on click, so it is the one string in this window
// that has to be right: it is where the window sends people.
const wchar_t* const kRepositoryUrl =
  L"https://github.com/nicoverbruggen/atelier-dusk-fixes";

const int kPageCount = 3;   // General, Graphics, About

// ---- the games -------------------------------------------------------------

// Each game ships two executables and the language decides which one runs: the
// `_EN` build is English, the other is the multilingual build carrying
// Japanese, Simplified Chinese and Traditional Chinese. Both are normally
// installed side by side, so which is present is not the question -- which to
// start is (see gameExeForLanguage).
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

// Which of the mod's own settings the running game actually has.
//
// This mirrors the capability matrix in src/core/game.cpp, which is the single
// source of truth; a cell that disagrees with it shows the user a control the
// DLL will refuse to act on. A row that is false here is NOT CREATED, so the
// window never offers a setting that would do nothing, and saving never grows a
// key in dusk-fix.ini for a game that ignores it.
//
// An earlier version of this file had no per-game knowledge at all and said so
// in a comment: every mod fix was on by default with no control here, so the
// window was identical for all three games. That stopped being true once the
// antialiasing settings arrived. Both of them are Ayesha-only, because the KTGL
// renderer has never been censused and nothing is known about what it binds.
struct Capabilities {
  bool supersampling;   // [Rendering] Supersampling
  bool smaa;            // [Rendering] SMAA
  bool startupSkips;    // [Startup] SkipLogos / SkipIntroMovie
};

const Capabilities kCapabilities[kGameCount] = {
  //             Ssaa   Smaa   Startup
  /* Ayesha  */ { true,  true,  true  },
  /* Escha   */ { false, false, false },
  /* Shallie */ { false, false, false },
};

char g_iniPath[MAX_PATH] = {};       // dusk-fix.ini, in the game folder
char g_settingsPath[MAX_PATH] = {};  // the game's own Setting.ini
char g_gameExePath[MAX_PATH] = {};   // an installed game exe, for the icon
char g_gameDir[MAX_PATH] = {};       // its folder, used as the working directory
const wchar_t* g_gameName = nullptr;
int g_game = -1;                     // index into kGames, -1 when none found

Capabilities capabilities() {
  if (g_game < 0 || g_game >= kGameCount)
    return Capabilities{ false, false, false };
  return kCapabilities[g_game];
}

// ---- combo box contents ----------------------------------------------------

struct ComboItem {
  const wchar_t* label;
  const char*    value;
};

struct Resolution { unsigned width; unsigned height; };

// Base (display) resolutions. The current desktop mode is appended at load time
// if it is not already here, as is whatever the game's own file already holds,
// so opening this window can never silently change a resolution it did not
// offer.
const Resolution kResolutions[] = {
  { 1280,  720 }, { 1366,  768 }, { 1600,  900 }, { 1920, 1080 },
  { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 },
};
const int kResolutionCount = int(sizeof(kResolutions) / sizeof(kResolutions[0]));

// Index 0 of the live list is always Auto, carried as a 0x0 sentinel exactly as
// the Arland launcher carries it.
constexpr Resolution kAutoResolution = { 0, 0 };

// The supersampling ladder, stored as percentages: a decimal in an ini is a
// locale trap, since a comma-decimal locale parses "1.5" as 1. Index 0 is Off.
const ComboItem kSsaaItems[] = {
  { L"Off",    "100" },
  { L"1.25x",  "125" },
  { L"1.5x",   "150" },
  { L"2x",     "200" },
  { L"3x",     "300" },
  { L"4x",     "400" },
};
const int kSsaaCount = 6;

// The ceiling the DLL clamps to, repeated here so the list never offers a
// multiplier that would silently be reduced. See supersample.cpp.
const unsigned kMaxRenderWidth = 7680;
const unsigned kMaxRenderHeight = 4320;

// There is deliberately no quality preset dropdown above these controls, and
// there used to be one. A preset here would set two adjacent controls, one of
// which writes the resulting resolution into its own labels, so it would say
// nothing the controls underneath do not. The Arland launcher dropped its own
// ladder for the same reason on the day multisampling was removed, and
// multisampling was one of the dimensions that had made this one worth having.
// Each control carries its cost beside it instead.

// The game's own [Window] FullScreen. There is no borderless entry: unlike the
// Arland mod this one has no window-mode override of its own, so the only two
// states are the two the game itself can be in.
const ComboItem kWindowModeItems[] = {
  { L"Windowed",   "0" },
  { L"Fullscreen", "1" },
};
const int kWindowModeCount = 2;

// The values Koei Tecmo's launcher compares against, and the build each one
// starts. Verified from the stock launcher's own string-compare chain rather
// than assumed; the chain itself is quoted in launcher_proxy.cpp.
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
HWND g_hSsaa = nullptr, g_hRendLbl = nullptr, g_hSmaa = nullptr;
HWND g_hOutline = nullptr;
HWND g_hSkipLogos = nullptr, g_hSkipMovie = nullptr;
HWND g_hSkipLauncher = nullptr;
HWND g_hStart = nullptr;
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
void repaintUnder(HWND ctrl);

// The base layout, in logical pixels. Sized for 720p at 100% so chooseScale
// always has an enlargement it can fall back from.
const int kBaseWidth = 700;
const int kBaseHeight = 440;

// Two scale factors, as in the Arland launcher. g_dpiScale is the display's,
// applied because the process is DPI aware and lays itself out in real pixels.
// g_userScale is ours, for a TV or handheld where the DPI-correct size is still
// too small to read across a room. Multiplying the font by both is the mistake
// this split exists to prevent: the system metrics already carry the DPI.
int g_dpiScale = 100;
int g_userScale = 100;
int S(int value) { return value * g_dpiScale / 100 * g_userScale / 100; }

// ---- ini reading -----------------------------------------------------------

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

// ---- ini writing, and knowing when it did not happen ------------------------
//
// Every write is checked. WritePrivateProfileStringA caches, so a failure can
// surface at any call or only at the flush that commits the file, and the user
// needs to be told which of the two files did not get written rather than which
// line failed. saveToIni clears these on entry and reports them back.
//
// What failed is kept alongside whether it failed, because "could not be
// written" on its own leaves nobody anywhere: the Win32 error separates a
// read-only file from a missing folder from something holding the file open.
struct WriteFailure {
  bool failed = false;
  DWORD error = 0;
  char where[64] = {};        // "Section/Key", or "flush" for the commit call
};
WriteFailure g_iniFailure;
WriteFailure g_settingsFailure;

// The last value this save committed to each file, kept so a reported failure
// can be checked against what is actually on disk. See verifyWrite below.
struct LastWrite {
  bool have = false;
  char section[32] = {};
  char key[32] = {};
  char value[64] = {};
};
LastWrite g_iniLastWrite;
LastWrite g_settingsLastWrite;

void noteLastWrite(LastWrite* last, const char* section, const char* key,
                   const char* value) {
  if (!section || !key || !value)
    return;                   // the flush carries no key to check
  last->have = true;
  lstrcpynA(last->section, section, sizeof(last->section));
  lstrcpynA(last->key, key, sizeof(last->key));
  lstrcpynA(last->value, value, sizeof(last->value));
}

bool iniWrite(const char* section, const char* key, const char* value,
              const char* path) {
  // No adopted game folder means no settings file to write, which is a normal
  // state rather than a failure. Writing there anyway would report a problem
  // the user cannot act on.
  if (!path || !path[0])
    return true;
  const bool settings = path == g_settingsPath;
  if (WritePrivateProfileStringA(section, key, value, path)) {
    noteLastWrite(settings ? &g_settingsLastWrite : &g_iniLastWrite,
                  section, key, value);
    return true;
  }
  WriteFailure* failure = settings ? &g_settingsFailure : &g_iniFailure;
  const DWORD error = GetLastError();
  // Keep the FIRST failure. It is the one that explains the rest, and a later
  // call overwriting it with a stale or cleared error is how this kind of
  // report ends up saying ERROR_SUCCESS.
  if (!failure->failed) {
    failure->failed = true;
    failure->error = error;
    if (section && key)
      wsprintfA(failure->where, "%.28s/%.28s", section, key);
    else
      lstrcpynA(failure->where, "flush", sizeof(failure->where));
  }
  return false;
}

void iniWriteBool(const char* path, const char* section, const char* key,
                  bool on) {
  iniWrite(section, key, on ? "true" : "false", path);
}

void iniWriteInt(const char* path, const char* section, const char* key,
                 unsigned value) {
  char text[16];
  wsprintfA(text, "%u", value);
  iniWrite(section, key, text, path);
}

// Did the write actually not happen? WritePrivateProfileStringA reporting
// failure and the value not reaching the file are different things, and under
// Wine the flush form (null section, null key) reports failure while every
// value written before it is on disk. Reporting a lost save that was not lost
// is the worse error of the two: it sends the user to check permissions on a
// folder that is fine, and it teaches them to ignore the warning.
//
// So a reported failure is checked against the file: read back the last value
// this save wrote and see whether it is there. Nothing to check against means
// trusting the report, which is the safe direction.
bool verifyWrite(const char* path, const LastWrite& last) {
  if (!path || !path[0] || !last.have)
    return false;
  char readBack[64] = {};
  GetPrivateProfileStringA(last.section, last.key, "\x01", readBack,
    sizeof(readBack), path);
  return readBack[0] != '\x01' && lstrcmpA(readBack, last.value) == 0;
}

// The launcher shares dusk-fix.log with the DLL rather than opening a second
// file: it is the file the user is asked for when reporting a problem, and a
// save failure is exactly the kind of thing that needs to be in it. Appended,
// never truncated, and every failure here is silent -- a tool that cannot write
// the ini very possibly cannot write the log either, and saying so twice helps
// nobody.
void appendToLog(const char* line) {
  if (!g_iniPath[0] || !line)
    return;
  char logPath[MAX_PATH];
  lstrcpynA(logPath, g_iniPath, MAX_PATH);
  const size_t len = std::strlen(logPath);
  if (len < 4 || len >= MAX_PATH)
    return;
  std::memcpy(logPath + len - 3, "log", 3);   // dusk-fix.ini -> .log
  const HANDLE file = CreateFileA(logPath, FILE_APPEND_DATA,
    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  WriteFile(file, line, (DWORD)std::strlen(line), &written, nullptr);
  CloseHandle(file);
}

// The short version of a Win32 error, for the ones that actually come up here.
// The number is always printed too, because the interesting case is the one
// this list does not cover.
const char* writeErrorName(DWORD error) {
  switch (error) {
    case ERROR_ACCESS_DENIED:    return "access denied (read-only file or folder)";
    case ERROR_FILE_NOT_FOUND:   return "file not found";
    case ERROR_PATH_NOT_FOUND:   return "path not found (folder missing)";
    case ERROR_SHARING_VIOLATION:return "file is open in another program";
    case ERROR_WRITE_PROTECT:    return "media is write protected";
    case ERROR_DISK_FULL:        return "disk full";
    case ERROR_SUCCESS:          return "the call reported failure without setting an error";
    default:                     return "see the Win32 error code";
  }
}

// One line per file per save, and only when something reported a failure.
// `verifiedOk` says whether the value was actually on disk afterwards: a
// reported failure that verified fine is logged as misreported, which is the
// line that explains a warning the user did not get.
void logSaveFailure(const char* name, const char* path,
                    const WriteFailure& failure, bool verifiedOk) {
  if (!failure.failed)
    return;
  char line[512];
  wsprintfA(line,
    "[launcher] %s write %s at %s: error %lu, %s (path %s)\r\n",
    name,
    verifiedOk ? "MISREPORTED (the value is on disk)" : "FAILED",
    failure.where, failure.error, writeErrorName(failure.error),
    path && path[0] ? path : "(none)");
  appendToLog(line);
}

// What saveToIni managed to write. Both files are written independently, so a
// partial failure leaves the resolution split across them, which is the state
// the two are kept in step to avoid.
struct SaveOutcome {
  bool ini = true;
  bool settings = true;
  bool ok() const { return ini && settings; }
};

// Name the file that did not get written. "Settings were saved" over a failed
// write is worse than the failure: the user has no way to tell it happened, and
// the most likely cause, a read-only dusk-fix.ini left behind by a Steam file
// verification, is something they can fix in a moment once they know.
void reportSaveFailure(HWND owner, SaveOutcome outcome) {
  if (outcome.ok())
    return;
  const wchar_t* which =
    !outcome.ini && !outcome.settings
      ? L"dusk-fix.ini and the game's own Setting.ini could not be written."
      : (!outcome.ini
           ? L"dusk-fix.ini could not be written."
           : L"The game's own Setting.ini could not be written, so the "
             L"resolution and language there are unchanged.");
  // Name the reason, not just the fact. The user cannot act on "could not be
  // written"; they can act on "read-only file or folder", and the code and the
  // failing key are what makes a report from someone else diagnosable.
  const WriteFailure& first =
    !outcome.ini && g_iniFailure.failed ? g_iniFailure : g_settingsFailure;
  wchar_t reason[192] = {};
  if (first.failed) {
    char detail[160];
    wsprintfA(detail, "%s (error %lu, writing %s)",
      writeErrorName(first.error), first.error, first.where);
    MultiByteToWideChar(CP_ACP, 0, detail, -1, reason, 160);
  }
  wchar_t text[768];
  wsprintfW(text,
    L"%s\n\n%s%sYour settings have not been saved.\n\n"
    L"The details are in dusk-fix.log beside the game.",
    which, reason[0] ? reason : L"", reason[0] ? L"\n\n" : L"");
  MessageBoxW(owner, text, L"Atelier Dusk Fixes", MB_OK | MB_ICONWARNING);
}

// ---- combo helpers ---------------------------------------------------------

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

bool isChecked(HWND ctrl) {
  return ctrl && SendMessageW(ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setChecked(HWND ctrl, bool on) {
  if (ctrl)
    SendMessageW(ctrl, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// ---- the resolution list ---------------------------------------------------

// Every entry the resolution combo currently offers, in the order shown. Held
// alongside the control because the list is not a fixed table: the desktop's
// own mode and the game file's existing value are appended when not already
// present.
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

// The largest mode the display reports. The resolution chosen here is what gets
// presented, so offering more than the panel has is offering a worse picture:
// the extra pixels are only scaled away again. Rendering higher than the screen
// is what the supersampling multiplier is for.
void displayMaximum(unsigned* width, unsigned* height) {
  *width = 0;
  *height = 0;
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
    if ((unsigned long long)mode.dmPelsWidth * mode.dmPelsHeight >
        (unsigned long long)*width * *height) {
      *width = mode.dmPelsWidth;
      *height = mode.dmPelsHeight;
    }
  }
  if (!*width || !*height)
    desktopResolution(width, height);
}

void addResolution(unsigned width, unsigned height) {
  for (const Resolution& r : g_resolutions) {
    if (r.width == width && r.height == height)
      return;
  }
  g_resolutions.push_back({ width, height });
}

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
      // supersampling list carries its render size: the answer belongs in the
      // list being chosen from, not somewhere the user has to go and look it up.
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

// The resolution the game will actually run at, which is what supersampling
// multiplies. Auto resolves to the desktop, exactly as saving would.
bool supersamplingBase(unsigned* width, unsigned* height) {
  const LRESULT index = SendMessageW(g_hRes, CB_GETCURSEL, 0, 0);
  if (index < 0 || size_t(index) >= g_resolutions.size())
    return false;
  const Resolution chosen = g_resolutions[size_t(index)];
  if (chosen.width && chosen.height) {
    *width = chosen.width;
    *height = chosen.height;
    return true;
  }
  return desktopResolution(width, height);
}

// ---- the supersampling list ------------------------------------------------

// The list holds only the multipliers that fit the current resolution, so its
// positions are not kSsaaItems positions; each item carries its own as item
// data. These are the only way to read and write the selection.
int ssaaSelectedIndex() {
  if (!g_hSsaa)
    return 0;   // "Off", which is also what the DLL does with an absent key
  const LRESULT at = SendMessageW(g_hSsaa, CB_GETCURSEL, 0, 0);
  if (at < 0)
    return 0;
  const LRESULT data = SendMessageW(g_hSsaa, CB_GETITEMDATA, WPARAM(at), 0);
  if (data < 0 || data >= kSsaaCount)
    return 0;
  return int(data);
}

// Select the entry for a kSsaaItems index, or Off when the list does not hold
// it (the resolution is too large for that multiplier).
void setSsaaIndex(int index) {
  if (!g_hSsaa)
    return;
  const int count = int(SendMessageW(g_hSsaa, CB_GETCOUNT, 0, 0));
  for (int at = 0; at < count; ++at) {
    if (int(SendMessageW(g_hSsaa, CB_GETITEMDATA, WPARAM(at), 0)) == index) {
      SendMessageW(g_hSsaa, CB_SETCURSEL, at, 0);
      return;
    }
  }
  SendMessageW(g_hSsaa, CB_SETCURSEL, 0, 0);
}

// Set when the multiplier that was selected (or loaded from the ini) does not
// fit the current resolution and was reduced to the largest one that does.
// Someone who asked for 4x at 4K wants as much supersampling as they can have,
// not none of it, so the cap lands them on 2x -- and it says so, because
// silently changing a setting and then writing it back on Save is how a
// configuration gets lost.
bool g_ssaaReduced = false;

// Select a kSsaaItems index, reducing rather than discarding. setSsaaIndex on
// its own lands on Off, which throws the setting away instead of honouring as
// much of it as fits, so every caller that means "restore this selection" comes
// through here.
void setSsaaIndexReducing(int index) {
  setSsaaIndex(index);
  g_ssaaReduced = false;
  if (g_hSsaa && index > 0 && ssaaSelectedIndex() != index) {
    // The list is built in ascending order, so its last entry is the largest
    // multiplier available for this resolution.
    const int count = int(SendMessageW(g_hSsaa, CB_GETCOUNT, 0, 0));
    if (count > 1) {
      SendMessageW(g_hSsaa, CB_SETCURSEL, count - 1, 0);
      g_ssaaReduced = true;
    }
  }
}

// Rebuild the list for the resolution now selected. Each entry carries the size
// it produces, so "what does 1.5x actually render at?" is answered in the list
// being chosen from rather than somewhere else in the window; anything past the
// 8K ceiling is left out rather than offered and then quietly reduced by the
// DLL.
void refillSupersampling() {
  // Null when the running game has no supersampling, and before the Graphics
  // page has been built.
  if (!g_hSsaa)
    return;
  const int wanted = ssaaSelectedIndex();

  unsigned baseWidth = 0, baseHeight = 0;
  const bool haveBase = supersamplingBase(&baseWidth, &baseHeight);
  SendMessageW(g_hSsaa, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < kSsaaCount; ++i) {
    unsigned renderWidth = 0, renderHeight = 0;
    if (i && haveBase) {
      // Bit for bit what ssaaSceneSize computes in src/core/supersample.cpp,
      // and deliberately so: truncating division, the ceiling tested on the
      // untruncated-to-even value, then the even mask. This used to round to
      // nearest and skip the mask, which made the size in the label something
      // the DLL never allocates -- two definitions of one number, which is the
      // failure this whole area was rewritten to stop repeating.
      const unsigned percent = unsigned(atoi(kSsaaItems[i].value));
      const unsigned exactWidth = (baseWidth * percent) / 100;
      const unsigned exactHeight = (baseHeight * percent) / 100;
      if (exactWidth > kMaxRenderWidth || exactHeight > kMaxRenderHeight)
        continue;
      renderWidth = exactWidth & ~1u;
      renderHeight = exactHeight & ~1u;
    }
    wchar_t label[96];
    if (i && haveBase)
      wsprintfW(label, L"%s  (%u x %u)", kSsaaItems[i].label, renderWidth,
        renderHeight);
    else
      lstrcpynW(label, kSsaaItems[i].label, 96);
    const int at = int(SendMessageW(g_hSsaa, CB_ADDSTRING, 0, (LPARAM)label));
    SendMessageW(g_hSsaa, CB_SETITEMDATA, at, LPARAM(i));
  }
  setSsaaIndexReducing(wanted);
}

// Rebuild the multiplier list and the note under it. Called after loading and
// after every change to the resolution.
void updateRenderResolution() {
  if (!g_hSsaa)
    return;
  unsigned baseWidth = 0, baseHeight = 0;
  const bool haveBase = supersamplingBase(&baseWidth, &baseHeight);
  refillSupersampling();
  EnableWindow(g_hSsaa, haveBase);
  if (!haveBase)
    SendMessageW(g_hSsaa, CB_SETCURSEL, 0, 0);   // force Off

  // The render sizes themselves are in the dropdown, so this row says only what
  // the dropdown cannot: why the multiplier that was configured is not the one
  // selected. Empty the rest of the time.
  char text[96] = "";
  if (g_ssaaReduced)
    lstrcpyA(text, "Reduced to fit the 8K limit.");
  else if (!haveBase)
    lstrcpyA(text, "Supersampling needs a resolution to work from.");
  SetWindowTextA(g_hRendLbl, text);
  // The only label whose text changes while the window is up, so the only one
  // that has to clear what it said before.
  repaintUnder(g_hRendLbl);
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
//
// Returns false when neither folder holds a recognised game, which the caller
// reports to the user.
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

// Create dusk-fix.ini for a folder that has the DLL but has never run the game.
// This is the same key src/core/config.cpp seeds in configPath() when it creates
// the file itself, so a launcher-made file and a DLL-made one are the same file.
// The per-feature keys are deliberately absent from both: featureEnabled() seeds
// those lazily from the per-game capability matrix, which this tool cannot see.
//
// False when the write fails, which is the only interesting outcome: the file is
// created by that write.
bool seedIniDefaults() {
  if (!g_iniPath[0])
    return false;
  return WritePrivateProfileStringA("Launcher", "SkipLauncher", "false",
    g_iniPath) != 0;
}

// ---- load, save, dirty tracking --------------------------------------------

// Everything the window can change, packed so two states can be compared. Used
// only to decide whether closing needs to ask. Snapshotting what saveToIni
// reads, rather than watching for change notifications, means no control can be
// missed and changing a setting back to its old value correctly counts as
// unchanged.
struct UiState {
  int  resolution;
  int  windowMode;
  int  language;
  int  ssaa;
  bool smaa;
  bool outline;
  bool skipLogos;
  bool skipMovie;
  bool skipLauncher;

  bool operator == (const UiState& o) const {
    return resolution == o.resolution && windowMode == o.windowMode &&
           language == o.language && ssaa == o.ssaa && smaa == o.smaa &&
           outline == o.outline && skipLogos == o.skipLogos &&
           skipMovie == o.skipMovie && skipLauncher == o.skipLauncher;
  }
};

UiState currentState() {
  UiState state = {};
  state.resolution = int(SendMessageW(g_hRes, CB_GETCURSEL, 0, 0));
  state.windowMode = int(SendMessageW(g_hWinMode, CB_GETCURSEL, 0, 0));
  state.language = int(SendMessageW(g_hLang, CB_GETCURSEL, 0, 0));
  state.ssaa = ssaaSelectedIndex();
  state.smaa = isChecked(g_hSmaa);
  state.outline = isChecked(g_hOutline);
  state.skipLogos = isChecked(g_hSkipLogos);
  state.skipMovie = isChecked(g_hSkipMovie);
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
  unsigned maxWidth = 0, maxHeight = 0;
  displayMaximum(&maxWidth, &maxHeight);
  for (int i = 0; i < kResolutionCount; ++i) {
    // Skip anything the display cannot show, as the Arland launcher does.
    if (maxWidth && maxHeight &&
        (kResolutions[i].width > maxWidth || kResolutions[i].height > maxHeight))
      continue;
    addResolution(kResolutions[i].width, kResolutions[i].height);
  }
  unsigned desktopWidth = 0, desktopHeight = 0;
  if (desktopResolution(&desktopWidth, &desktopHeight))
    addResolution(desktopWidth, desktopHeight);
  // Whatever the game's file already holds is offered too, even when it is
  // larger than this display. That is the one place this list is wider than
  // Arland's, and it is deliberate: rendering above the panel is how the
  // 1440p render-target census was measured on a 1080p screen, and a
  // deliberate downsampling setup must survive being looked at. Nothing is
  // added that the user did not already choose.
  if (width && height)
    addResolution(width, height);
  // Auto is the launcher's own memory of a choice, not a value the game could
  // hold, so it lives in dusk-fix.ini rather than in Setting.ini.
  //
  // The two projects have to resolve Auto in different places. Arland leaves
  // its ini keys blank and its DLL decides what they mean when the device is
  // created, so Auto there follows the display even if it changes between
  // launches. This mod writes a literal number into the game's own Setting.ini,
  // because the game reads that field itself and this mod deliberately adds no
  // resolution override to duplicate it -- and "Auto" is not something the
  // game's integer parse can be handed. So Auto is resolved at save time and
  // the choice is remembered here.
  //
  // Its default is what a file that has never been saved gets, and it has to be
  // Auto to match the Arland launcher, which selects Auto whenever its own ini
  // carries no resolution. The default is conditional rather than a flat `true`,
  // though, because a flat true would break the other rule this window keeps:
  // opening it must never silently replace a resolution the user already chose.
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
  // font-atlas read cache, the two field-jitter halves, the high-resolution
  // correction, the travel-map cursor, the synthesis animation rate, the
  // system-save guard and the loading-text correction. None of them is a
  // choice. The high-resolution one is the only one that had to be argued
  // rather than assumed -- rendering at 4K costs real performance -- but
  // choosing the resolution IS that decision, and the fix only makes the
  // resolution already chosen honest. Each has an environment switch for the
  // A/B a bug report needs.
  //
  // What is left is the two antialiasing settings, and both exist only where
  // the capability matrix says the game has them.
  if (capabilities().smaa)
    setChecked(g_hSmaa, iniBool(g_iniPath, "Rendering", "SMAA", false));
  if (capabilities().supersampling) {
    iniString(g_iniPath, "Rendering", "Supersampling", value, sizeof(value),
      "100");
    const int percent = atoi(value);
    int index = 0;
    for (int i = 0; i < kSsaaCount; ++i) {
      if (atoi(kSsaaItems[i].value) == percent) {
        index = i;
        break;
      }
    }
    // The list has to hold the multipliers for this resolution before one can
    // be selected. A saved value the resolution no longer allows is reduced to
    // the largest that fits, not discarded.
    refillSupersampling();
    setSsaaIndexReducing(index);
  }

  // [Startup]: both off by default, and Ayesha-only.
  if (capabilities().startupSkips) {
    setChecked(g_hSkipLogos, iniBool(g_iniPath, "Startup", "SkipLogos", false));
    setChecked(g_hSkipMovie,
      iniBool(g_iniPath, "Startup", "SkipIntroMovie", false));
  }

  setChecked(g_hSkipLauncher,
    iniBool(g_iniPath, "Launcher", "SkipLauncher", false));

  // Last, once every quality control holds its loaded value.
  updateRenderResolution();
  markSaved();
}

SaveOutcome saveToIni() {
  g_iniFailure = WriteFailure{};
  g_settingsFailure = WriteFailure{};
  g_iniLastWrite = LastWrite{};
  g_settingsLastWrite = LastWrite{};

  const LRESULT index = SendMessageW(g_hRes, CB_GETCURSEL, 0, 0);
  if (index >= 0 && size_t(index) < g_resolutions.size()) {
    Resolution chosen = g_resolutions[size_t(index)];
    const bool automatic = !chosen.width && !chosen.height;
    // Auto is resolved to a literal here, because the game's own field can only
    // hold a number. If the display cannot be read, fall back to 1080p rather
    // than writing a zero the game would pass straight through: its reader only
    // replaces a NEGATIVE value with a default.
    if (automatic && !desktopResolution(&chosen.width, &chosen.height))
      chosen = { 1920, 1080 };
    iniWriteInt(g_settingsPath, "Graphics", "ScreenWidth", chosen.width);
    iniWriteInt(g_settingsPath, "Graphics", "ScreenHeight", chosen.height);
    iniWriteBool(g_iniPath, "Launcher", "AutoResolution", automatic);
  }
  iniWrite("Window", "FullScreen",
    comboValue(g_hWinMode, kWindowModeItems, kWindowModeCount), g_settingsPath);
  iniWrite("Lang", "Language",
    comboValue(g_hLang, kLangItems, kLangCount), g_settingsPath);
  iniWrite("Graphics", "Outline", isChecked(g_hOutline) ? "1" : "0",
    g_settingsPath);

  // Written only where the game has the feature, so a game the DLL would refuse
  // it for never grows the key. That is the same rule featureEnabled() follows
  // on the other side of the ini.
  if (capabilities().smaa)
    iniWriteBool(g_iniPath, "Rendering", "SMAA", isChecked(g_hSmaa));
  if (capabilities().supersampling)
    iniWrite("Rendering", "Supersampling",
      kSsaaItems[ssaaSelectedIndex()].value, g_iniPath);

  if (capabilities().startupSkips) {
    iniWriteBool(g_iniPath, "Startup", "SkipLogos", isChecked(g_hSkipLogos));
    iniWriteBool(g_iniPath, "Startup", "SkipIntroMovie",
      isChecked(g_hSkipMovie));
  }

  iniWriteBool(g_iniPath, "Launcher", "SkipLauncher",
    isChecked(g_hSkipLauncher));

  // Flush the cache so each file is on disk before we report success.
  iniWrite(nullptr, nullptr, nullptr, g_iniPath);
  iniWrite(nullptr, nullptr, nullptr, g_settingsPath);

  // A reported failure is checked against the file before it becomes a warning,
  // and every failure is logged either way: the ones that turn out to be real
  // need the Win32 error to be diagnosable at all, and the ones that do not are
  // worth knowing about because they mean the platform is misreporting.
  SaveOutcome outcome;
  outcome.ini = !g_iniFailure.failed ||
                verifyWrite(g_iniPath, g_iniLastWrite);
  outcome.settings = !g_settingsFailure.failed ||
                     verifyWrite(g_settingsPath, g_settingsLastWrite);
  logSaveFailure("dusk-fix.ini", g_iniPath, g_iniFailure, outcome.ini);
  logSaveFailure("Setting.ini", g_settingsPath, g_settingsFailure,
                 outcome.settings);
  return outcome;
}

// Put every setting back to a known-good starting point: the mod's own
// defaults, plus Auto windowed.
//
// Language is the one exception, and deliberately so. It is not tuning that can
// be wrong -- it is what the player reads the game in, and resetting someone to
// English because they wanted their graphics settings back would be a hostile
// reading of "defaults". saveToIni writes it back unchanged from its control.
//
// The resolution deliberately differs from the Arland launcher, which resets to
// 1280x720 as a safe floor it knows every display can show. Auto is that floor
// here and a better one: it resolves to the desktop mode, which is by
// definition displayable, and this launcher has to write a literal number into
// the game's file either way. A fresh install looking far worse than the screen
// it is running on is the outcome "defaults" should not produce.
void resetToDefaults() {
  unsigned width = 1920, height = 1080;
  if (desktopResolution(&width, &height))
    addResolution(width, height);
  refillResolutions(width, height, true);

  SendMessageW(g_hWinMode, CB_SETCURSEL, 0, 0);   // windowed
  setChecked(g_hOutline, true);                   // on as the game shipped

  // Off, not a recommended setting. Reset is for getting back to a known state,
  // and a reset that quietly switched supersampling and SMAA on would cost
  // frame rate that nobody asked to spend -- on the weakest machine running
  // this, which is where reset is most likely to be reached for.
  setChecked(g_hSmaa, false);
  setSsaaIndex(0);
  setChecked(g_hSkipLogos, false);
  setChecked(g_hSkipMovie, false);
  setChecked(g_hSkipLauncher, false);
  updateRenderResolution();
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
  const SaveOutcome saved = saveToIni();
  if (!saved.ok()) {
    // Launching now would run with settings that were never written, which is
    // the confusing outcome: the game ignores what is on screen and nothing
    // says why. Offer the choice rather than deciding it.
    reportSaveFailure(w, saved);
    if (MessageBoxW(w, L"Start the game anyway, with the settings that are "
                       L"already in the file?", L"Atelier Dusk Fixes",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
      return false;
  } else {
    // Nothing is pending any more, so a failed launch leaves no close prompt.
    markSaved();
  }

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
HWND mkLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
  HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
    x, y, w, h, parent, nullptr, nullptr, nullptr);
  setFont(c);
  return c;
}

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
  if (!ctrl)
    return;
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

  // A line belonging to the control above it, so it starts at the control
  // column rather than the label margin.
  void under(HWND control) {
    MoveWindow(control, controlLeft(), y,
      fullWidth() - (controlLeft() - left()), labelHeight(), TRUE);
    onPage(page, control);
    y += labelHeight() + S(10);
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
  const wchar_t* pageNames[kPageCount] = { L"General", L"Graphics", L"About" };
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

  // ---------------- page 0: General ----------------
  {
    Layout page(w, 0);
    g_hLang = mkCombo(w, 0, 0, 10, IDC_LANG);
    comboFill(g_hLang, kLangItems, kLangCount);
    page.row(L"Language:", g_hLang,
      L"Written to the game's own settings, and decides which of the game's "
      L"two executables starts.");

    g_hRes = mkCombo(w, 0, 0, 10, IDC_RES);
    page.row(L"Resolution:", g_hRes,
      L"What reaches the screen. Written to the game's own settings, which is "
      L"where this one lives.");

    g_hWinMode = mkCombo(w, 0, 0, 10, IDC_WINMODE);
    comboFill(g_hWinMode, kWindowModeItems, kWindowModeCount);
    page.row(L"Window mode:", g_hWinMode,
      L"Fullscreen takes over the display; windowed does not, so alt-tab is "
      L"instant.");

    // A statement of fact rather than a setting, so it spans the width.
    page.fullNote(runningUnderWine()
      ? L"The game runs at your display's refresh rate, 120 Hz and 144 Hz "
        L"included. The mod does not cap the frame rate, so a limit set by "
        L"Steam or the compositor is respected."
      : L"The game runs at your display's refresh rate, 120 Hz and 144 Hz "
        L"included. The mod does not cap the frame rate.");

    // [Startup]. Ayesha-only: the two KTGL games run a different boot path and
    // neither the logo object nor the movie routine has a homolog there.
    if (capabilities().startupSkips) {
      page.heading(L"Startup");
      g_hSkipLogos = mkCheck(w, L"Skip the startup logos", 0, 0, 10,
        IDC_SKIPLOGOS);
      page.checkRow(g_hSkipLogos,
        L"The logos play while the game loads, so this shows a black screen "
        L"for as long as loading takes rather than starting sooner.");
      g_hSkipMovie = mkCheck(w, L"Skip the opening movie", 0, 0, 10,
        IDC_SKIPMOVIE);
      page.checkRow(g_hSkipMovie,
        L"Goes straight to the title screen. The ending and event movies still "
        L"play, but the opening cannot be replayed from the Movies menu while "
        L"this is on.");
    }

    // [Launcher] SkipLauncher. Read by the 32-bit msimg32 proxy rather than by
    // the DLL, and about the launch Steam performs rather than the window in
    // front of you, which is the half of it that is easy to get wrong.
    page.heading(L"Launcher");
    g_hSkipLauncher = mkCheck(w, L"Skip this window when launching from Steam",
      0, 0, 10, IDC_SKIPLAUNCHER);
    page.checkRow(g_hSkipLauncher,
      L"Play in Steam goes straight into the game with the settings already "
      L"saved here. Run dusk-fix-launcher.exe to get back to this window.");
  }

  // ---------------- page 1: Graphics ----------------
  // These are the settings with a frame-rate cost, grouped so that cost is in
  // one place instead of scattered through settings that have none.
  {
    Layout page(w, 1);
    const Capabilities caps = capabilities();

    if (caps.supersampling) {
      g_hSsaa = mkCombo(w, 0, 0, 10, IDC_SSAA);
      page.row(L"Supersampling:", g_hSsaa,
        L"Renders higher, then scales down. The sharpest, and the costliest. "
        L"Limited to 8K.");

      // The live readout under the supersampling row. Registered as a note so
      // it draws in the secondary colour, but it is not created by note(): its
      // text changes at runtime, so it keeps a fixed height rather than being
      // measured once against a string it will not be showing later.
      g_hRendLbl = mkLabel(w, L"", 0, 0, 10, 10);
      if (g_descCount < 32)
        g_hDesc[g_descCount++] = g_hRendLbl;
      page.under(g_hRendLbl);
    }

    if (caps.smaa) {
      g_hSmaa = mkCheck(w, L"Edge smoothing", 0, 0, 10, IDC_SMAA);
      page.checkRow(g_hSmaa,
        L"Cheap, and smooths edges inside textures as well as along the edges "
        L"of models.");
    }

    // The game's own, and present in all three titles' Setting.ini.
    g_hOutline = mkCheck(w, L"Character outlines", 0, 0, 10, IDC_OUTLINE);
    page.checkRow(g_hOutline,
      L"The game's own outline rendering. On as it shipped.");

    // Said plainly rather than left as an empty page. Escha & Logy and Shallie
    // run a renderer this mod has not censused, so it knows nothing about which
    // surface carries their 3D scene -- and every one of its image-quality
    // features needs that answer before it can touch anything.
    if (!caps.supersampling && !caps.smaa)
      page.fullNote(
        L"The mod's own image-quality settings are not available for this "
        L"game yet. Supersampling and edge smoothing need to know which part "
        L"of the frame carries the 3D scene, and that has only been "
        L"established for Atelier Ayesha so far.");
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

    // The stock front-ends are still reachable: this tool replaces them, it
    // does not remove them. They sit here rather than among the settings
    // because they lead out of this window instead of changing anything in it.
    // Greyed out when the executable is not present.
    page.heading(L"The game as it shipped");
    HWND openEnv = mkButton(w, L"Settings &editor", 0, 0, 10, IDC_OPENENV);
    HWND openLauncher = mkButton(w, L"&Original launcher", 0, 0, 10,
      IDC_OPENLAUNCHER);
    HWND playVanilla = mkButton(w, L"Play &without the mod", 0, 0, 10,
      IDC_PLAYVANILLA);
    page.buttons(openEnv, openLauncher, playVanilla);
    if (!stockToolPresent(g_game >= 0 ? kGames[g_game].stockEnv : nullptr))
      EnableWindow(openEnv, FALSE);
    if (!stockToolPresent(g_game >= 0 ? kGames[g_game].stockLauncher : nullptr))
      EnableWindow(openLauncher, FALSE);
    if (!g_gameExePath[0])
      EnableWindow(playVanilla, FALSE);
    page.fullNote(
      L"Koei Tecmo's own settings editor and launcher, unmodified. The third "
      L"saves and starts the game with the mod turned off, changing nothing.");
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
  // E editor, O original launcher, W play without the mod.
  HWND reset = CreateWindowExW(0, L"BUTTON", L"&Reset to defaults",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    rightEdge - closeW - S(12) - wideW, buttonTop, wideW, buttonH, w,
    (HMENU)(INT_PTR)IDC_RESET, nullptr, nullptr);
  setFont(reset);

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
      // Every static and checkbox stands on a tab page, so all of them get the
      // page's colour -- the theme's on Windows, the flat white under Wine.
      // This is what stops them reading as patches laid over the panel.
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
      // The supersampling list is computed from the selected resolution, so it
      // has to follow every change to it.
      if (LOWORD(wp) == IDC_RES && HIWORD(wp) == CBN_SELCHANGE) {
        updateRenderResolution();
        return 0;
      }
      switch (LOWORD(wp)) {
        case IDC_RESET: {
          // Destructive and not undoable, so it asks first, and the question
          // names what it will and will not touch. Defaulting to No: this sits
          // next to Close, and the cost of a mis-click is someone's whole
          // configuration.
          const int answer = MessageBoxW(w,
            L"Reset all of the mod's settings to their defaults? This will "
            L"also set your game resolution back to your desktop's, in "
            L"windowed mode. Your language is left alone.",
            L"Atelier Dusk Fixes",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
          if (answer != IDYES)
            return 0;
          resetToDefaults();
          const SaveOutcome reset = saveToIni();
          if (!reset.ok()) {
            // No markSaved: the values are on screen but not on disk, so the
            // close prompt has to stay armed.
            reportSaveFailure(w, reset);
            return 0;
          }
          markSaved();
          // Name the game back to the user: this tool configures whichever
          // folder it sits in, and saying which one closes that loop.
          wchar_t saved[320];
          wsprintfW(saved,
            L"The settings have been reset to the mod's defaults and saved. "
            L"The next time you launch %s they will be used.",
            g_gameName ? g_gameName : L"the game");
          MessageBoxW(w, saved, L"Atelier Dusk Fixes",
            MB_OK | MB_ICONINFORMATION);
          return 0;
        }
        case IDC_START:
        case IDC_PLAYVANILLA:
          if (startGame(w, LOWORD(wp) == IDC_PLAYVANILLA))
            DestroyWindow(w);
          return 0;
        case IDC_OPENLAUNCHER:
        case IDC_OPENENV: {
          // Saved first, so the stock tool opens onto the settings on screen
          // rather than the ones on disk. Both of them edit the same
          // Setting.ini this window does.
          const SaveOutcome saved = saveToIni();
          if (!saved.ok())
            reportSaveFailure(w, saved);
          else
            markSaved();
          const bool env = LOWORD(wp) == IDC_OPENENV;
          const char* tool = g_game < 0 ? nullptr
            : (env ? kGames[g_game].stockEnv : kGames[g_game].stockLauncher);
          if (!runStockTool(tool)) {
            wchar_t failed[256];
            wsprintfW(failed,
              L"%s could not be started. It may have been moved or removed "
              L"from the game folder.",
              env ? L"The settings editor" : L"The original launcher");
            MessageBoxW(w, failed, L"Atelier Dusk Fixes",
              MB_OK | MB_ICONWARNING);
          }
          return 0;
        }
        case IDC_CLOSE:
        // IsDialogMessage turns Escape into IDCANCEL, so this is the Escape
        // key. Both go through WM_CLOSE so the unsaved-changes check sits in
        // one place, shared with the window's own close button.
        case IDCANCEL:
          SendMessageW(w, WM_CLOSE, 0, 0);
          return 0;
        default:
          break;
      }
      break;

    case WM_CLOSE:
      // Settings are written when the game starts, so closing after an edit
      // would throw it away with no sign that it happened. Cancel is the
      // default: of the three answers it is the only one that loses nothing.
      if (hasUnsavedChanges()) {
        const int answer = MessageBoxW(w,
          L"Save the changes you made to the settings before closing? They "
          L"are normally written when you start the game.",
          L"Atelier Dusk Fixes",
          MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (answer == IDCANCEL)
          return 0;
        if (answer == IDYES)
          reportSaveFailure(w, saveToIni());
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

  // Fixed-size dialog-style window (no maximize / resize). Declared here
  // because the scale has to know the frame it will be measured with.
  const DWORD style =
    (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX);
  chooseScale(style);
  // Before the font and the brushes: both are chosen by the regime it picks.
  initStyling();
  g_uiFont = createUiFont();
  g_headingFont = createHeadingFont();
  g_windowBrush = CreateSolidBrush(g_windowBack);
  g_pageBrush = CreateSolidBrush(g_pageBack);

  // This tool edits the files beside it, so a game must be there. Saying where
  // to put it is the whole of the diagnosis for a misplaced copy. An earlier
  // version opened the window anyway and reported the problem in a message box,
  // which left a settings window on screen that could not start anything and
  // whose Save wrote into a folder no game reads.
  if (!resolveGameFolder()) {
    MessageBoxW(nullptr,
      L"No Atelier Dusk game was found in this folder.\n\n"
      L"Put dusk-fix-launcher.exe in the game's installation folder, beside "
      L"the game executable and d3d11.dll, and run it from there.",
      L"Atelier Dusk Fixes", MB_OK | MB_ICONERROR);
    return 1;
  }
  // A missing dusk-fix.ini is not a reason to stop. Deleting the ini to start
  // over, or copying the launcher into a folder before ever running the game,
  // would otherwise leave this window editing a file that does not exist.
  // Create it with the same key src/core/config.cpp seeds in configPath(), so a
  // launcher-made file and a DLL-made one are the same file. Only a failure to
  // write it is worth stopping for, and that is a real problem the user can act
  // on.
  if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES &&
      !seedIniDefaults()) {
    MessageBoxW(nullptr,
      L"dusk-fix.ini is missing and could not be created in this folder.\n\n"
      L"The game folder may be read-only, which a Steam file verification can "
      L"leave behind. Check the folder's permissions and try again.",
      L"Atelier Dusk Fixes", MB_OK | MB_ICONERROR);
    return 1;
  }

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

  // Centred rather than left to the default cascade position. This window is
  // the first thing seen when the game is started, so it should arrive where
  // the eye already is. Centred on the work area of the monitor holding the
  // cursor, so a taskbar cannot push the lower buttons off-screen.
  RECT area = {};
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  if (cursorWorkArea(&area)) {
    x = area.left + (area.right - area.left - frameWidth) / 2;
    y = area.top + (area.bottom - area.top - frameHeight) / 2;
  }

  // Name the game in the title, not just in the window: with the game's icon
  // beside it this is what identifies the right one on a taskbar holding more
  // than one of these.
  wchar_t title[192];
  if (g_gameName)
    wsprintfW(title, L"%s - Atelier Dusk Fixes", g_gameName);
  else
    lstrcpynW(title, L"Atelier Dusk Fixes", 192);

  HWND window = CreateWindowExW(0, cls.lpszClassName, title,
    style, x, y, frameWidth, frameHeight, nullptr, nullptr, instance, nullptr);
  if (!window)
    return 1;

  ShowWindow(window, show);
  UpdateWindow(window);
  // After the controls exist and the window is up, so nothing takes it back.
  // Skipped when there is no game to start, since focus on a disabled control
  // would leave the keyboard nowhere.
  if (g_hStart && IsWindowEnabled(g_hStart))
    SetFocus(g_hStart);

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
