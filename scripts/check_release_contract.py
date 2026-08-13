#!/usr/bin/env python3
"""Keep source attribution and third-party release licensing intact."""

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def fail(message):
    print(f"release contract check failed: {message}", file=sys.stderr)
    return 1


def main():
    try:
        meson = (ROOT / "meson.build").read_text(encoding="utf-8")
        main_cpp = (ROOT / "src" / "core" / "main.cpp").read_text(
            encoding="utf-8"
        )
        stamp = (ROOT / "src" / "core" / "version_git.h.in").read_text(
            encoding="utf-8"
        )
        workflow = (ROOT / ".github" / "workflows" / "build.yml").read_text(
            encoding="utf-8"
        )
        license_text = (ROOT / "LICENSE").read_text(encoding="utf-8")

        for fragment in (
            "version_git_header = vcs_tag(",
            "'git', '-c', 'safe.directory=*', 'describe'",
            "'--dirty=-dirty'",
            "fallback : 'unknown'",
        ):
            if fragment not in meson:
                raise ValueError("Meson source stamp is missing: " + fragment)
        if (
            '#include "version_git.h"' not in main_cpp
            or "DUSK_FIX_GIT" not in main_cpp
        ):
            raise ValueError("the game DLL no longer logs the generated source stamp")
        if "#define DUSK_FIX_GIT \"@VCS_TAG@\"" not in stamp:
            raise ValueError("version_git.h.in no longer publishes @VCS_TAG@")

        package_line = (
            "cp vendor/smaa/LICENSE.txt dist/dusk-fix/LICENSES/SMAA.txt"
        )
        if package_line not in workflow:
            raise ValueError(
                "the release archive no longer packages the SMAA licence"
            )
        if "LICENSES/SMAA.txt in the release archive" not in license_text:
            raise ValueError("LICENSE no longer names the packaged SMAA licence")
    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print("release contract ok: source stamp and SMAA licence packaging agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
