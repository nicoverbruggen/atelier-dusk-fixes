#!/usr/bin/env python3
"""Check the settings launcher's defaults against the defaults the code uses.

No ini ships any more. The values a user meets before the DLL has ever written a
file are the launcher's own, so those are what this checks, and it checks them by
running the launcher rather than by reading it. `--write-defaults <game> <path>`
writes exactly what a fresh Save would write, so a launcher that shows the wrong
value cannot pass by carrying a table that says the right one.

Run once per game, because the defaults differ per game. That is also what
replaces the old comparison of the launcher's capability copy against the matrix:
a wrong cell now shows up as a key that is present when the matrix says the game
does not have it, or absent when it says it does.

Checked, per game:
  * every key the launcher writes is actually read by the code
  * every key the code reads is written, unless it is allowed below
  * where the code's default is a literal, the two values agree
  * where it comes from the capability matrix, the cell and the file agree,
    including a key being absent exactly when the cell is U

Needs Windows, since it runs the launcher. Set DUSK_LAUNCHER_RUNNER to a command
prefix to check from elsewhere, for example `umu-run`.
"""

import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SECTIONS = ("Rendering", "Startup", "Interface", "Launcher", "Diagnostics",
            "Debug")

# The launcher's argument for each game, in capability-matrix row order.
GAMES = ("ayesha", "escha", "shallie")

# Options the code reads that a default run of the launcher does not write.
#
# Two shapes end up here and they are not the same thing, so every entry says
# which it is:
#
#   "exposed ..."      a control offers it, but its default state writes no key.
#                      An unset combo, or a value the window resolves at save
#                      time rather than storing. Nothing is hidden.
#   "not exposed ..."  no control offers it, so it can only be set by someone
#                      who already knows the name. The surface is the launcher,
#                      so this is a decision that the option is not for players,
#                      not a note that its control has not been written yet.
NOT_WRITTEN_AT_DEFAULT: dict[tuple[str, str], str] = {
    ("Debug", "SlopeHold"):
        "exposed on the Debug page as a disable, and written only when it is"
        " ticked. The fix is on as shipped, so the default state writes no key."
        " The page itself is only on the tab strip when verbose logging is on",
    ("Rendering", "DisplayWidth"):
        "exposed as the resolution control, and written only when that is not"
        " Auto. Auto is the default",
    ("Rendering", "DisplayHeight"):
        "exposed as the resolution control, and written only when that is not"
        " Auto. Auto is the default",
    ("Interface", "SteadyControlPrompt"):
        "not exposed at all, on purpose. Shallie's control-prompt hold is set"
        " by hand or not at all. The startup log resolves the feature on"
        " Shallie, which seeds the key, so it is in the file after one run",
    ("Interface", "HideControlPrompt"):
        "not exposed at all, on purpose, and the second half of the same"
        " choice. Seeded once SteadyControlPrompt is on, which is when it"
        " starts meaning anything",
}

# Keys where the launcher and the code differ on purpose, because they are not
# answering the same question. Each needs the reason, not just the exemption.
DIFFERENT_BY_DESIGN: dict[tuple[str, str], str] = {
    ("Launcher", "AutoResolution"):
        "the launcher defaults the choice to Auto; the msimg32 proxy reads a"
        " missing key as false so that it never rewrites the game's own"
        " resolution for a choice nobody made",
}

# Matrix rows carrying no ini key of their own, because the option is a valued
# knob rather than a switch and has its own reader. The cell still says which
# games have the feature, and that is what decides whether the key belongs in a
# given game's file, so each is matched to its row by the row's environment name.
KEYLESS_ROWS = {
    ("Rendering", "ShadowMultiplier"): "DUSK_SHADOW_MULTIPLIER",
}

SECTION_ALT = "|".join(SECTIONS)
PATTERNS = (
    # duskConfigBool("Section", "Key", true)
    re.compile(rf'duskConfigBool\("({SECTION_ALT})",\s*"(\w+)",\s*(true|false)\)'),
    # duskConfigInt("Section", "Key", 100)
    re.compile(rf'duskConfigInt\("({SECTION_ALT})",\s*"(\w+)",\s*(\d+)\)'),
    # WritePrivateProfileStringA("Section", "Key", "value", path) -- the seeding
    # in configPath(), which writes an option's default.
    re.compile(rf'WritePrivateProfileStringA\("({SECTION_ALT})",\s*"(\w+)",\s*"([^"]*)"'),
    # GetPrivateProfileStringW(L"Section", L"Key", L"default", ...). The launcher
    # proxy reads wide: its ini sits beside the game, and a Steam library path
    # can hold characters the ANSI code page cannot represent.
    re.compile(rf'GetPrivateProfileStringW\(L"({SECTION_ALT})",\s*L"(\w+)",\s*L"([^"]*)"'),
)

# Reads whose default is elsewhere, recorded for presence only: a non-literal
# fallback, and game.cpp's descriptor rows, whose defaults come from the matrix.
KEY_ONLY = (
    re.compile(rf'duskConfig(?:Bool|Int)\("({SECTION_ALT})",\s*"(\w+)"'),
    re.compile(rf'GetPrivateProfile\w+A\("({SECTION_ALT})",\s*"(\w+)"'),
    re.compile(rf'"DUSK_\w+",\s*"({SECTION_ALT})",\s*"(\w+)"'),
)


def parse_retired():
    """(section, key) pairs config.cpp reports as retired.

    Guarded because the list is a claim about the code: a key listed there and
    still read by src/ would have the log telling a player it does nothing while
    it quietly went on working.
    """
    text = (ROOT / "src" / "core" / "config.cpp").read_text(encoding="utf-8")
    block = re.search(r"kRetiredKeys\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not block:
        return None
    return set(re.findall(r'\{\s*"(\w+)",\s*"(\w+)"\s*\}', block.group(1)))


def parse_ini(path):
    """(section, key) -> value, ignoring comments and blank lines."""
    values = {}
    section = None
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif "=" in line and section:
            key, _, value = line.partition("=")
            values[(section, key.strip())] = value.strip()
    return values


def sources():
    """Every .cpp under src/, at any depth: this project splits by engine."""
    return sorted(ROOT.glob("src/**/*.cpp"))


def parse_source():
    """(section, key) -> default from the source, or None if not a literal."""
    defaults = {}
    for source in sources():
        text = source.read_text(encoding="utf-8")
        for pattern in PATTERNS:
            for section, key, value in pattern.findall(text):
                defaults.setdefault((section, key), value)
        for pattern in KEY_ONLY:
            for section, key in pattern.findall(text):
                defaults.setdefault((section, key), None)
    return defaults


def parse_matrix():
    """Per game: (section, key) -> ("X" | "O" | "U").

    game.cpp holds a descriptor row per feature (env name, ini section, ini key)
    and three matrix rows, one per game, where X is on by default, O is opt-in
    and U is unsupported. featureEnabled() turns a cell into the key's default,
    so the cell is the default for that game and no key has to be skipped for
    differing between them.
    """
    text = (ROOT / "src" / "core" / "game.cpp").read_text(encoding="utf-8")
    rows = re.findall(
        r'\{\s*"(DUSK_[A-Z_0-9]+)",\s*(?:"(\w+)"|nullptr),\s*(?:"(\w+)"|nullptr)\s*\}',
        text,
    )
    grids = [
        [c.strip() for c in m.split(",") if c.strip()]
        for m in re.findall(r"constexpr Support k\w+\[\]\s*=\s*\{([^}]*)\}", text)
    ]
    grids = [g for g in grids if g and all(c in ("X", "O", "U") for c in g)]
    if len(grids) != len(GAMES) or len({len(g) for g in grids}) != 1:
        return None

    per_game = [{} for _ in GAMES]
    columns = {}
    for index, (env, section, key) in enumerate(rows):
        if index >= len(grids[0]):
            continue
        columns[env] = index
        if not section or not key:
            continue
        for game, grid in enumerate(grids):
            per_game[game][(section, key)] = grid[index]

    for entry, env in KEYLESS_ROWS.items():
        index = columns.get(env)
        if index is None:
            return None
        for game, grid in enumerate(grids):
            per_game[game][entry] = grid[index]
    return per_game


def write_defaults(launcher, game, out):
    """Run the launcher headless. Returns an error string, or None on success."""
    runner = shlex.split(os.environ.get("DUSK_LAUNCHER_RUNNER", ""))
    command = runner + [str(launcher), "--write-defaults", game, str(out)]
    try:
        result = subprocess.run(command, capture_output=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"could not run the launcher for {game}: {error}"
    if result.returncode != 0:
        return f"the launcher exited {result.returncode} for {game}"
    if not out.exists():
        return f"the launcher wrote no file for {game}"
    return None


def same(written, expected):
    """Compare an ini value with a source default across the two spellings."""
    if expected in ("true", "false") or written in ("true", "false"):
        truthy = {"true", "1", "yes", "on"}
        return (written.lower() in truthy) == (expected.lower() in truthy)
    if written.isdigit() and expected.isdigit():
        return int(written) == int(expected)
    return written == expected


def compare(game, ini, source, cells):
    problems = []

    for entry, value in sorted(ini.items()):
        if entry not in source:
            problems.append(
                f"{game}: the launcher writes [{entry[0]}] {entry[1]}, "
                "which nothing in src/ reads"
            )
            continue
        cell = cells.get(entry)
        if cell == "U":
            problems.append(
                f"{game}: the launcher writes [{entry[0]}] {entry[1]}, "
                "but the capability matrix says this game does not have it"
            )
            continue
        # A keyless row's cell says whether the game has the option, not what it
        # is set to: those are valued knobs with their own reader in src/.
        from_matrix = cell is not None and entry not in KEYLESS_ROWS
        expected = {"X": "true", "O": "false"}[cell] if from_matrix else source[entry]
        if entry in DIFFERENT_BY_DESIGN:
            continue
        if expected is not None and not same(value, expected):
            where = "the capability matrix" if from_matrix else "src/"
            problems.append(
                f"{game}: [{entry[0]}] {entry[1]} is {value!r} from the "
                f"launcher and {expected!r} from {where}"
            )

    for entry in sorted(source):
        if entry in ini or entry in NOT_WRITTEN_AT_DEFAULT:
            continue
        if cells.get(entry) == "U":
            continue          # correctly absent: this game does not have it
        problems.append(
            f"{game}: [{entry[0]}] {entry[1]} is read by src/ but the launcher "
            "does not write it, so nobody can find it"
        )

    return problems


def main():
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <path to dusk-fix-launcher.exe>",
              file=sys.stderr)
        return 2
    launcher = Path(sys.argv[1])
    if not launcher.exists():
        print(f"no launcher at {launcher}", file=sys.stderr)
        return 2

    source = parse_source()
    per_game = parse_matrix()
    if per_game is None:
        print("the capability matrix in src/core/game.cpp could not be read",
              file=sys.stderr)
        return 1

    retired = parse_retired()
    if retired is None:
        print("the retired-key list in src/core/config.cpp could not be read",
              file=sys.stderr)
        return 1

    # A key cannot be both retired and read. The log would be telling a player
    # it does nothing while it went on working.
    problems = [
        f"[{section}] {key} is listed as retired but src/ still reads it"
        for section, key in sorted(retired & set(source))
    ]

    with tempfile.TemporaryDirectory() as tmp:
        for index, game in enumerate(GAMES):
            out = Path(tmp) / f"{game}.ini"
            failure = write_defaults(launcher, game, out)
            if failure:
                problems.append(failure)
                continue
            problems += compare(game, parse_ini(out), source, per_game[index])

    for entry, reason in sorted(NOT_WRITTEN_AT_DEFAULT.items()):
        print(f"note: [{entry[0]}] {entry[1]}: {reason}")
    for entry, reason in sorted(DIFFERENT_BY_DESIGN.items()):
        print(f"note: [{entry[0]}] {entry[1]} differs on purpose: {reason}")

    if problems:
        print("\nThe launcher and src/ disagree about the mod's defaults:\n",
              file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        print("\nFix whichever is wrong, or add the option to NOT_WRITTEN_AT_DEFAULT "
              "in this script with the reason it cannot be offered.",
              file=sys.stderr)
        return 1

    print(f"\nok: {len(GAMES)} games checked against src/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
