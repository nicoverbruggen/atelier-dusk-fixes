#!/usr/bin/env python3
"""Keep Batch 4's central hook and address-mutation invariants in CI."""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
HOOK_H = (ROOT / "src" / "core" / "hook_util.h").read_text()
HOOK_CPP = (ROOT / "src" / "core" / "hook_util.cpp").read_text()
D3D = (ROOT / "src" / "core" / "d3d11_hooks.cpp").read_text()
FIELD = (ROOT / "src" / "engines" / "phyre" / "field_physics.cpp").read_text()
PROTECTION = (ROOT / "src" / "core" / "protection_transaction.h").read_text()
PROMPT = (ROOT / "src" / "engines" / "ktgl" / "control_prompt_fix.cpp").read_text()
MAIN = (ROOT / "src" / "core" / "main.cpp").read_text()
GAME_H = (ROOT / "src" / "core" / "game.h").read_text()
MESON = (ROOT / "meson.build").read_text()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    try:
        for token in (
            "class HookTransaction", "TargetCollision", "DisableRollback",
            "RemoveRollback", "kMaxHooks", "kMaxPublications",
        ):
            require(token in HOOK_H, f"hook transaction header is missing {token}")
        for token in (
            "MH_RemoveHook", "clearPublications", "transaction.rollback()",
            "transaction.commit()",
        ):
            require(token in HOOK_CPP, f"hook transaction implementation is missing {token}")

        require("std::lock_guard<atfix::mutex> installLock" in D3D,
                "central D3D install is not serialized")
        require("HookTransaction transaction" in D3D and
                "transaction.enableAll()" in D3D and
                "transaction.rollback()" in D3D,
                "central D3D install bypasses the owned transaction")
        require("MH_ERROR_ALREADY_CREATED" not in D3D,
                "central D3D install broadly accepts ALREADY_CREATED")
        require(D3D.index("CreateDeferredContext") <
                D3D.index("HookTransaction transaction"),
                "deferred-context acquisition happens after hook creation starts")
        require("g_installPoisoned" in D3D and "ROLLBACK INCOMPLETE" in D3D,
                "incomplete central rollback is not reported and quarantined")

        for path in (
            ROOT / "src" / "core" / "d3d11_probe.cpp",
            ROOT / "src" / "core" / "d3d11_probe.h",
        ):
            require(not path.exists(), f"spent second vtable owner still exists: {path.name}")
        for text, label in ((MAIN, "main.cpp"), (GAME_H, "game.h"),
                            (MESON, "meson.build")):
            require("d3d11_probe" not in text and "D3D11WriteProbe" not in text,
                    f"{label} still wires the spent D3D11 probe")

        for token in ("ProtectionTransaction protection", "protection.rollback()",
                      "protection.commit()", "rollback_incomplete"):
            require(token in FIELD, f"field page rollback is missing {token}")
        for token in ("ProtectProc", "originalProtection_", "rollback()"):
            require(token in PROTECTION,
                    f"page-protection transaction is missing {token}")
        require("matches(draw, kDrawExpected)" in PROMPT,
                "control-prompt Draw hook lacks a verified prologue")
        require("HookTransaction transaction" in PROMPT and
                PROMPT.count("transaction.create(") >= 2 and
                "transaction.enableAll()" in PROMPT,
                "control-prompt Hide is not one two-hook transaction")
    except (OSError, ValueError) as exc:
        print(f"transaction contract check failed: {exc}", file=sys.stderr)
        return 1
    print("transaction contract ok: central hooks and address rollback invariants hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
