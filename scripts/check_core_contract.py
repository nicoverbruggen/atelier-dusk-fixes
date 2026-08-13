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
SHARPEN = (ROOT / "src" / "core" / "sharpen.cpp").read_text()
SMAA = (ROOT / "src" / "core" / "smaa.cpp").read_text()
SSAA = (ROOT / "src" / "core" / "supersample.cpp").read_text()
HIGHRES = (ROOT / "src" / "core" / "highres.cpp").read_text()
SCENE_PASS = (ROOT / "src" / "core" / "scene_pass.cpp").read_text()
KTGL_PRE_UI = (
    ROOT / "src" / "engines" / "ktgl" / "scene_target.cpp"
).read_text()
PHYRE_PRE_UI = (
    ROOT / "src" / "engines" / "phyre" / "pre_ui.cpp"
).read_text()
PRESENT_CLAMP = (
    ROOT / "src" / "engines" / "ktgl" / "present_clamp.cpp"
).read_text()


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
        notify = phyre_init.find("startPadNotifyTrace();")
        success = phyre_init.rfind("return true;")
        if notify < 0 or success < notify:
            raise ValueError(
                "Phyre initialization success no longer covers controller/diagnostic fan-out"
            )
        if re.search(r"if\s*\(\s*!wantAddressFix\s*\)\s*return false", phyre_init):
            raise ValueError(
                "Phyre initialization again depends on the address-patch subset"
            )

        # Fullscreen passes run inside the engine's frame. Sharpening must put
        # back every render target it can displace, while every pass must skip
        # rather than draw with stale constants after a failed WRITE_DISCARD.
        for fragment in (
            "OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs",
            "D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv",
            "for (auto*& rtv : rtvs) release(rtv)",
            "D3D11_COLOR_WRITE_ENABLE_RED |",
            "D3D11_COLOR_WRITE_ENABLE_GREEN |",
            "D3D11_COLOR_WRITE_ENABLE_BLUE",
        ):
            if fragment not in SHARPEN:
                raise ValueError(
                    "sharpening no longer preserves every render target: "
                    + fragment
                )
        for label, source in (
            ("sharpening", SHARPEN),
            ("SMAA", SMAA),
            ("supersampling", SSAA),
        ):
            if "if (FAILED(mapResult))" not in source:
                raise ValueError(
                    f"{label} no longer skips the pass after a failed map"
                )

        # SMAA clears and binds through t9, so all ten slots belong to the state
        # bracket. Its reusable targets are keyed by concrete format as well as
        # size, and every pre-UI caller goes through the shared mode switch.
        for fragment in (
            "ID3D11ShaderResourceView* srvs[10]",
            "PSGetShaderResources(0, 10, srvs)",
            "PSSetShaderResources(0, 10, srvs)",
            "viewFormat != g_format",
            "if (!smaaPreUiEnabled() || !ctx || !scene || g_broken)",
        ):
            if fragment not in SMAA:
                raise ValueError("SMAA render-state contract is missing: " + fragment)

        # Editable ini values are untrusted at the DLL boundary. They remain
        # inside the launcher's ceiling and may only reduce either requested
        # swap-chain component, never enlarge its partner.
        for fragment in (
            "kMaxConfiguredWidth = 7680",
            "kMaxConfiguredHeight = 4320",
            "std::min(wasWidth, UINT(displayWidth))",
            "std::min(wasHeight, UINT(displayHeight))",
        ):
            if fragment not in PRESENT_CLAMP:
                raise ValueError(
                    "KTGL present clamp no longer enforces its final bound: "
                    + fragment
                )

        # Width/height publications are indivisible, and a refused enlarged
        # allocation is never hidden by returning one original-size member of
        # an otherwise enlarged target family.
        for fragment in (
            "std::atomic<uint64_t> g_mainSize{0}",
            "std::atomic<uint64_t> g_swapSize{0}",
            "g_mainSize.compare_exchange_strong",
            "g_swapSize.store(packSize(width, height), std::memory_order_release)",
            "returning the failure without an incompatible",
        ):
            if fragment not in HIGHRES:
                raise ValueError(
                    "high-resolution publication/failure contract is missing: "
                    + fragment
                )
        if HIGHRES.count(
            "createTexture2D(self, desc, initialData, texture)"
        ) != 1:
            raise ValueError(
                "high-resolution path has gained an original-descriptor retry"
            )

        # Deferred-context state follows the context object, and Finish mirrors
        # D3D's restore-state contract instead of unconditionally dropping the
        # SSAA marker.
        for label, source, iid in (
            ("raster", HIGHRES, "IID_DuskHighResRasterDirty"),
            ("KTGL pre-UI", KTGL_PRE_UI, "IID_DuskKtglPreUiState"),
            ("Phyre pre-UI", PHYRE_PRE_UI, "IID_DuskPhyrePreUiState"),
        ):
            for fragment in (iid, "SetPrivateData", "GetPrivateData"):
                if fragment not in source:
                    raise ValueError(
                        f"{label} state no longer follows its D3D context: {fragment}"
                    )
        finish = function_body(
            SCENE_PASS,
            "HRESULT STDMETHODCALLTYPE hookedFinishCommandList(",
            "FinishCommandList detour",
        )
        require(
            r"if\s*\(SUCCEEDED\(result\)\s*&&\s*!restoreState\)\s*"
            r"ssaaClearContextState\(self\)",
            finish,
            "FinishCommandList no longer preserves the marker with restoreState=TRUE",
        )

        # Each shared fullscreen resource tuple is serialized and tied to the
        # one measured device. SSAA's substituted view crosses the guard only
        # with a retained reference that the forwarding detour releases.
        for label, source in (("SMAA", SMAA), ("SSAA", SSAA)):
            for fragment in (
                "std::atomic<bool> g_passBusy{false}",
                "ID3D11Device* g_ownerDevice = nullptr",
                "acceptsDevice(",
            ):
                if fragment not in source:
                    raise ValueError(
                        f"{label} shared-resource ownership is missing: {fragment}"
                    )
        for fragment in (
            "g_smallSRV->AddRef()",
            "substituted[i] != views[i]",
            "substituted[i]->Release()",
        ):
            if fragment not in SSAA + SCENE_PASS:
                raise ValueError("SSAA retained handoff is missing: " + fragment)

    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print(
        "core contract ok: atomic proxy publication, bounded system path, "
        "On12 forwarding, exact-build D3D gating, and render-pass safety agree"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
