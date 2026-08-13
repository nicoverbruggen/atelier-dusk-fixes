#!/usr/bin/env python3
"""Check the Dusk launcher proxy and GUI's mutation-safety contract."""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
GUI = (ROOT / "src" / "launcher" / "launcher_gui.cpp").read_text()
PROXY = (ROOT / "src" / "launcher" / "launcher_proxy.cpp").read_text()
INI_WRITES = (ROOT / "src" / "launcher" / "ini_write_set.h").read_text()


def require(pattern, text, label):
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"could not find {label}")
    return match


def main():
    try:
        table = require(
            r"constexpr std::array<DuskGame, 3> SupportedGames = \{\{(.*?)\}\};",
            PROXY, "proxy game table",
        ).group(1)
        for launcher in (
            "Atelier_AyeshaLauncher.exe",
            "Atelier_Escha_and_LogyLauncher.exe",
            "Atelier_ShallieLauncher.exe",
        ):
            if launcher not in table:
                raise ValueError(f"proxy game table is missing {launcher}")
        if table.count("0xe8,0x7f,0xe6") != 3:
            raise ValueError("not every Dusk launcher has an entry byte window")
        if table.count("\n    false,") != 1 or table.count("\n    true,") != 2:
            raise ValueError("proxy game table lost the two KTGL SSAA policies")

        # Auto resolution must rebuild KTGL's multiplied render size from the
        # desktop base and saved factor. Reading the already-multiplied game ini
        # back as the base would compound the scale on every redirected start.
        for token in (
            "void applyAutoResolution(bool ssaaScalesGameIni)",
            "candidateWidth = uint64_t(width) * candidate / 100",
            "candidateHeight = uint64_t(height) * candidate / 100",
            "L\"DisplayWidth\", value, ini.data()",
            "L\"DisplayHeight\", value, ini.data()",
            "applyAutoResolution(game->ssaaScalesGameIni)",
        ):
            if token not in PROXY:
                raise ValueError(
                    "redirected auto-resolution is no longer idempotent: " + token
                )

        arm = require(r"bool armRedirect\(\) \{(.*?)\n\}", PROXY,
                      "armRedirect").group(1)
        if "game->launcherEntryExpected" not in arm or \
                "std::memcmp(g_entryPoint" not in arm:
            raise ValueError("proxy does not verify the per-title entry window")
        if arm.index("std::memcmp(g_entryPoint") > arm.index("VirtualProtect(g_entryPoint"):
            raise ValueError("proxy makes the entry writable before verifying its bytes")
        for token in (
            "kLauncherEntryRelocationOffset = 13",
            "kLauncherPreferredImageBase = 0x00400000",
            "relocateEntryWindow(base, expectedEntry)",
        ):
            if token not in PROXY:
                raise ValueError(f"launcher entry verification is not ASLR-aware: {token}")
        if PROXY.count("0x00 }") < 3:
            raise ValueError("Dusk launcher entry windows do not cover the full relocation")

        original = require(
            r"void runOriginalEntryPoint\(\) \{(.*?)\n\}", PROXY,
            "runOriginalEntryPoint",
        ).group(1)
        if "if (!VirtualProtect" not in original or "ExitProcess(1)" not in original:
            raise ValueError("entry restore failure can recurse into the redirect")

        if "LastWrite" in GUI or "verifyWrite(" in GUI:
            raise ValueError("GUI reverted to last-key-only save verification")
        for token in ("g_iniWrites.verify", "g_settingsWrites.verify"):
            if token not in GUI:
                raise ValueError(f"GUI does not exhaustively verify {token}")
        for token in ("for (size_t i = 0; i < count_; ++i)",
                      "entries_[i].deleted", "GetPrivateProfileStringA"):
            if token not in INI_WRITES:
                raise ValueError(f"INI write-set helper is missing {token}")

        stock = require(
            r"case IDC_OPENLAUNCHER:(.*?)case IDC_CLOSE:", GUI,
            "stock-tool command",
        ).group(1)
        failure = stock.find("if (!saved.ok())")
        run = stock.find("runStockTool")
        if failure < 0 or run < 0 or "return 0;" not in stock[failure:run]:
            raise ValueError("stock tool can launch after its prerequisite save failed")

        close = require(r"case WM_CLOSE:(.*?)case WM_DESTROY:", GUI,
                        "WM_CLOSE handler").group(1)
        if not re.search(
            r"const SaveOutcome saved = saveToIni\(\);.*?"
            r"if \(!saved\.ok\(\)\) \{.*?return 0;.*?\}.*?DestroyWindow",
            close, re.DOTALL,
        ):
            raise ValueError("failed close-time save can still destroy the window")
    except (OSError, ValueError) as exc:
        print(f"launcher contract check failed: {exc}", file=sys.stderr)
        return 1
    print("launcher contract ok: entry patching and exhaustive save persistence hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
