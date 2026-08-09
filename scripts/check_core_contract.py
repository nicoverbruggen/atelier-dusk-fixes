#!/usr/bin/env python3
"""Protect the Dusk proxy's forwarding and exact-recognition boundary.

This is a source-level structural check, not a substitute for booting the three
games. It keeps the unknown-build pass-through path, procedure publication and
compatibility export from drifting during later rendering work.
"""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MAIN = (ROOT / "src" / "core" / "main.cpp").read_text()
KTGL = (ROOT / "src" / "engines" / "ktgl" / "ktgl.cpp").read_text()
PHYRE = (ROOT / "src" / "engines" / "phyre" / "phyre.cpp").read_text()


def fail(message):
    print(f"core contract check failed: {message}", file=sys.stderr)
    return 1


def function_body(text, signature, label):
    start = text.find(signature)
    if start < 0:
        raise ValueError(f"could not find {label}")
    opening = text.find("{", start + len(signature))
    if opening < 0:
        raise ValueError(f"could not find body of {label}")
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]
    raise ValueError(f"unterminated body of {label}")


def require(pattern, text, message):
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(message)
    return match


def check_passthrough(body, member, mutations):
    guard = require(
        rf"if\s*\(!dusk::initializeEngineFixes\(\)\)\s*"
        rf"return\s*\(\*proc\.{member}\)\(",
        body,
        f"{member} no longer has an immediate unknown-build pass-through",
    )
    for mutation in mutations:
        position = body.find(mutation)
        if position < 0:
            raise ValueError(f"{member} lost expected wiring: {mutation}")
        if position < guard.end():
            raise ValueError(
                f"{member} performs {mutation} before exact-build gating"
            )


def main():
    try:
        loader = function_body(MAIN, "D3D11Proc loadSystemD3D11()", "D3D11 loader")
        require(
            r"std::atomic<bool>\s+ready\s*\{\s*false\s*\}",
            loader,
            "D3D11 procedure table has no independent atomic ready flag",
        )
        require(
            r"ready\.load\(std::memory_order_acquire\)",
            loader,
            "D3D11 loader fast path is not acquire-synchronized",
        )
        publish = require(
            r"ready\.store\(true,\s*std::memory_order_release\)",
            loader,
            "D3D11 procedure table is not release-published",
        )
        on12_lookup = require(
            r"GetProcAddress\(libD3D11,\s*\"D3D11On12CreateDevice\"\)",
            loader,
            "D3D11On12CreateDevice is not resolved from the real DLL",
        )
        if publish.start() < on12_lookup.end():
            raise ValueError("D3D11 procedure table is published before it is complete")
        if "strncat" in loader:
            raise ValueError("system D3D11 path uses unbounded strncat semantics")
        require(
            r"length\s*\+\s*sizeof\(suffix\)\s*>\s*path\.size\(\)",
            loader,
            "system D3D11 path append is not bounded against the full suffix",
        )

        create = function_body(
            MAIN,
            "DLLEXPORT HRESULT __stdcall D3D11CreateDevice(",
            "D3D11CreateDevice",
        )
        check_passthrough(
            create,
            "D3D11CreateDevice",
            ("hookFactoryForSwapChain", "d3d11InstallHooks"),
        )

        create_swap = function_body(
            MAIN,
            "DLLEXPORT HRESULT __stdcall D3D11CreateDeviceAndSwapChain(",
            "D3D11CreateDeviceAndSwapChain",
        )
        check_passthrough(
            create_swap,
            "D3D11CreateDeviceAndSwapChain",
            (
                "ssaaClampPresentSize", "noteSwapChainSize",
                "ssaaFitOutputWindow", "ssaaNoteBackBuffer", "hookPresent",
                "d3d11InstallHooks",
            ),
        )

        on12 = function_body(
            MAIN,
            "DLLEXPORT HRESULT __stdcall D3D11On12CreateDevice(",
            "D3D11On12CreateDevice",
        )
        require(
            r"return\s+proc\.D3D11On12CreateDevice\(",
            on12,
            "D3D11On12CreateDevice is not forwarded",
        )
        for forbidden in (
            "initializeEngineFixes", "d3d11InstallHooks", "hookPresent",
            "ssaaClampPresentSize",
        ):
            if forbidden in on12:
                raise ValueError(
                    f"D3D11On12CreateDevice is no longer pass-through-only: {forbidden}"
                )

        ktgl_init = function_body(
            KTGL, "bool initializeKtglFixes()", "KTGL initializer"
        )
        mismatch = require(
            r"if\s*\(!verified\)\s*\{(.*?)\n\s*\}",
            ktgl_init,
            "KTGL fingerprint mismatch branch is missing",
        ).group(1)
        if "return false;" not in mismatch:
            raise ValueError("KTGL fingerprint mismatch authorizes shared D3D fixes")

        phyre_init = function_body(
            PHYRE, "bool initializePhyreFixes()", "Phyre initializer"
        )
        recognition = require(
            r"g_game\s*=\s*recognizeExecutable\(g_base\);\s*"
            r"if\s*\(!g_game\)\s*return false;",
            phyre_init,
            "Phyre initializer does not reject an unrecognized executable first",
        )
        registration = phyre_init.find("registerPhyreSceneTarget();")
        if registration < recognition.end():
            raise ValueError("Phyre scene policy is registered before exact recognition")

    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print(
        "core contract ok: atomic proxy publication, bounded system path, "
        "On12 forwarding, and exact-build D3D gating agree"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
