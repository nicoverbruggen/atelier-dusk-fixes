#!/usr/bin/env python3
"""Check default.ini against the defaults the code actually uses.

default.ini is what users get in the release archive, renamed to dusk-fix.ini,
and it repeats values that really live in src/. It is now the only description of the
option surface, and a stale shipped file misrepresents the mod to exactly the people
least able to notice. This compares them and fails if they disagree.

Checked in both directions:
  * every option the code reads appears in default.ini
  * every option in default.ini is actually read by the code
  * where the default is a literal in the source, the values match
  * the settings launcher's own fallbacks agree with default.ini, since it has
    to show a value before the DLL has ever written one

Ported from the sibling Arland project's script of the same name. The idioms
differ enough to matter: this project reads through duskConfigBool/duskConfigInt,
its feature descriptors are three fields rather than four, its capability matrix
is three separate arrays, and its launcher takes the ini path as the first
argument of every reader.

Run from the repository root, or via `python3 scripts/check_default_ini.py`.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SECTIONS = ("Rendering", "Startup", "Interface", "Launcher")

# Options the code reads that are deliberately kept out of default.ini. Empty
# today: every option the code reads carries one shipped value that is correct
# on all three games, so default.ini can state it. An option belongs here only
# when no single value would be right -- writing the line would then change
# behaviour on at least one game, and default.ini promises that deleting any
# line changes nothing.
UNDOCUMENTED: set[tuple[str, str]] = set()

SECTION_ALT = "|".join(SECTIONS)
PATTERNS = (
    # duskConfigBool("Section", "Key", true)
    re.compile(rf'duskConfigBool\("({SECTION_ALT})",\s*"(\w+)",\s*(true|false)\)'),
    # duskConfigInt("Section", "Key", 100)
    re.compile(rf'duskConfigInt\("({SECTION_ALT})",\s*"(\w+)",\s*(\d+)\)'),
    # WritePrivateProfileStringA("Section", "Key", "value", path) -- the seeding
    # that makes an option discoverable, which writes its default.
    re.compile(rf'WritePrivateProfileStringA\("({SECTION_ALT})",\s*"(\w+)",\s*"([^"]*)"'),
    # GetPrivateProfileStringW(L"Section", L"Key", L"default", ...). The launcher
    # proxy reads wide: its ini sits beside the game, and a Steam library path
    # can hold characters the ANSI code page cannot represent.
    re.compile(rf'GetPrivateProfileStringW\(L"({SECTION_ALT})",\s*L"(\w+)",\s*L"([^"]*)"'),
)

# Reads whose default is elsewhere, recorded for presence only. Two shapes: a
# non-literal default (duskConfigInt with a named fallback), and game.cpp's
# feature descriptor table, whose rows are { env, section, key } and whose
# defaults come from the per-game matrix rather than the call site.
KEY_ONLY = (
    re.compile(rf'duskConfig(?:Bool|Int)\("({SECTION_ALT})",\s*"(\w+)"'),
    re.compile(rf'GetPrivateProfile\w+A\("({SECTION_ALT})",\s*"(\w+)"'),
    re.compile(rf'"DUSK_\w+",\s*"({SECTION_ALT})",\s*"(\w+)"'),
)

# The launcher keeps its own copy of every default, because it has to show a
# value before the DLL has ever run. That copy drifts silently: it is a separate
# file with a separate idiom, and a launcher that disagrees does not just
# display the wrong thing, it writes the wrong thing back on the next Save.
#
# Every reader here takes the ini path first, which is what makes these patterns
# differ from the DLL's above.
LAUNCHER_PATTERNS = (
    # iniBool(path, "Section", "Key", true)
    re.compile(rf'iniBool\(\w+,\s*"({SECTION_ALT})",\s*"(\w+)",\s*(true|false)\)'),
    # iniString(path, "Section", "Key", buf, sizeof(buf), "default")
    re.compile(
        rf'iniString\(\w+,\s*"({SECTION_ALT})",\s*"(\w+)",[^;]*?"([^"]*)"\s*\)'
    ),
)


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


def parse_matrix_defaults():
    """Defaults that come from the capability matrix rather than a literal.

    game.cpp holds a descriptor row per feature (env name, ini section, ini key)
    and three matrix rows, one per game, where X is on by default, O is opt-in
    and U is unsupported. featureEnabled() turns a cell into the key's default.
    A feature the matrix supports differently across games has no single correct
    value, so it is skipped -- and if it is also absent from default.ini it must
    be named in UNDOCUMENTED, or the presence check above will catch it.
    """
    text = (ROOT / "src" / "core" / "game.cpp").read_text(encoding="utf-8")
    rows = re.findall(
        r'\{\s*"DUSK_[A-Z_0-9]+",\s*(?:"(\w+)"|nullptr),\s*(?:"(\w+)"|nullptr)\s*\}',
        text,
    )
    grids = [
        [c.strip() for c in m.split(",") if c.strip()]
        for m in re.findall(r"constexpr Support k\w+\[\]\s*=\s*\{([^}]*)\}", text)
    ]
    grids = [g for g in grids if g and all(c in ("X", "O", "U") for c in g)]
    if len(grids) != 3 or len({len(g) for g in grids}) != 1:
        return {}

    defaults = {}
    for index, (section, key) in enumerate(rows):
        if not section or not key or index >= len(grids[0]):
            continue
        cells = {g[index] for g in grids if g[index] != "U"}
        if len(cells) != 1:
            continue          # genuinely per-game; no single shipped default
        defaults[(section, key)] = "true" if cells.pop() == "X" else "false"
    return defaults


def per_game_launcher_problems():
    """Check the launcher's per-game copy of a matrix cell against the matrix.

    A key in UNDOCUMENTED has no single shipped value, so neither the default.ini
    comparison nor the launcher comparison above can reach it -- and that is
    exactly the key whose launcher fallback is most likely to be wrong, because
    the window shows and writes it before the DLL has ever seen the file. The
    launcher therefore carries the cell itself, in kCapabilities, and this
    compares that column against game.cpp row by row.
    """
    game = (ROOT / "src" / "core" / "game.cpp").read_text(encoding="utf-8")
    gui = (ROOT / "src" / "launcher" / "launcher_gui.cpp").read_text(
        encoding="utf-8"
    )

    rows = re.findall(
        r'\{\s*"DUSK_[A-Z_0-9]+",\s*(?:"(\w+)"|nullptr),\s*(?:"(\w+)"|nullptr)\s*\}',
        game,
    )
    grids = [
        [c.strip() for c in m.split(",") if c.strip()]
        for m in re.findall(r"constexpr Support k\w+\[\]\s*=\s*\{([^}]*)\}", game)
    ]
    grids = [g for g in grids if g and all(c in ("X", "O", "U") for c in g)]

    body = re.search(r"struct Capabilities \{(.*?)\n\};", gui, re.DOTALL)
    table = re.search(
        r"const Capabilities kCapabilities\[kGameCount\] = \{(.*?)\n\};", gui,
        re.DOTALL,
    )
    if len(grids) != 3 or not body or not table:
        return ["the capability matrix or the launcher's copy could not be read"]

    fields = re.findall(r"^\s*bool (\w+);", body.group(1), re.MULTILINE)
    cells = [
        [c.strip() for c in group.split(",") if c.strip()]
        for group in re.findall(r"\{([^{}]*)\}", table.group(1))
    ]
    if len(cells) != 3:
        return ["the launcher's kCapabilities table is not three rows"]

    # field in the launcher's struct -> (section, key) it mirrors.
    MIRRORED = {"smaaOnByDefault": ("Rendering", "SMAA")}
    names = ("Ayesha", "Escha & Logy", "Shallie")
    problems = []
    for field, entry in MIRRORED.items():
        if field not in fields:
            problems.append(f"the launcher no longer carries {field}")
            continue
        column = fields.index(field)
        try:
            feature = [(s, k) for s, k in rows].index(entry)
        except ValueError:
            problems.append(f"{entry[0]}/{entry[1]} is not in game.cpp's descriptors")
            continue
        for game_index, name in enumerate(names):
            if column >= len(cells[game_index]) or feature >= len(grids[game_index]):
                problems.append(f"{name}: {field} has no cell to compare")
                continue
            expected = "true" if grids[game_index][feature] == "X" else "false"
            actual = cells[game_index][column]
            if actual != expected:
                problems.append(
                    f"{name}: the matrix makes {entry[0]}/{entry[1]} default to "
                    f"{expected!r}, the launcher's {field} says {actual!r}"
                )
    return problems


def parse_launcher():
    """(section, key) -> default value the settings launcher falls back to."""
    defaults = {}
    text = (ROOT / "src" / "launcher" / "launcher_gui.cpp").read_text(
        encoding="utf-8"
    )
    for pattern in LAUNCHER_PATTERNS:
        for match in pattern.finditer(text):
            defaults.setdefault(
                (match.group(1), match.group(2)), match.groups()[-1]
            )
    return defaults


def main():
    ini = parse_ini(ROOT / "default.ini")
    source = parse_source()
    launcher = parse_launcher()
    problems = []

    for entry in sorted(set(source) - set(ini) - UNDOCUMENTED):
        problems.append(
            f"{entry[0]}/{entry[1]}: read by the code but missing from default.ini"
        )
    for entry in sorted(set(ini) - set(source)):
        problems.append(
            f"{entry[0]}/{entry[1]}: in default.ini but never read by the code"
        )
    for entry in sorted(UNDOCUMENTED & set(ini)):
        problems.append(
            f"{entry[0]}/{entry[1]}: allowlisted as undocumented, but default.ini "
            f"gives it a value -- remove one or the other"
        )

    matrix_defaults = parse_matrix_defaults()
    for entry in sorted(set(ini) & set(source)):
        expected = source[entry]
        if expected is None:
            expected = matrix_defaults.get(entry)
        if expected is None:
            continue
        if ini[entry].lower() != expected.lower():
            problems.append(
                f"{entry[0]}/{entry[1]}: default.ini says {ini[entry]!r}, "
                f"the code defaults to {expected!r}"
            )

    for entry in sorted(set(ini) & set(launcher)):
        if ini[entry].lower() != launcher[entry].lower():
            problems.append(
                f"{entry[0]}/{entry[1]}: default.ini says {ini[entry]!r}, "
                f"the settings launcher falls back to {launcher[entry]!r}"
            )

    problems.extend(per_game_launcher_problems())

    if problems:
        print("default.ini disagrees with the code:\n", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\nUpdate default.ini to match, or adjust the "
            "allowlist in this script if an option is deliberately undocumented.",
            file=sys.stderr,
        )
        return 1

    print(f"default.ini agrees with the code ({len(ini)} options checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
