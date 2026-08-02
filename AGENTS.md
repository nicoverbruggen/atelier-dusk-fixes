# AGENTS.md

## Project scope

Fixes for the Steam Dusk-trilogy DX ports: Atelier Ayesha DX, Atelier Escha & Logy DX, Atelier Shallie DX. This project is still pre-release; read `WORK_DOC.md` for the detailed current evidence and `../atelier-re-tools/DUSK.md` for the concise work queue.

## Source layout

Split by engine, because the trilogy spans two and they share nothing a fix can reach:

- `src/core/` — engine-agnostic: the D3D11 proxy (`main.cpp`), engine dispatch (`engine.cpp`), the capability matrix (`game.cpp`), the `dusk-fix.ini` layer (`config.cpp`), D3D11 vtable ownership and hook installation (`d3d11_hooks.cpp`), the high-resolution fix and its render-target census (`highres.cpp`), the antialiasing features (`msaa.cpp`, `smaa.cpp`, `supersample.cpp`), logging.

  **`d3d11_hooks.cpp` is the only place that hooks a D3D11 vtable.** Features own their detours and their policy and declare them in a "wiring for d3d11_hooks.cpp" section of their own header; they do not call MinHook. Two modules hooking one vtable is how an enable/disable race gets written by accident, and how a half-installed set survives a failure that should have rolled everything back. Add a slot by adding a row to a spec table there, not by hooking from a feature.

  Rendering features may still need one engine-specific decision each. Keep it out of core: `src/core/msaa.h` takes a `MsaaSceneTest` callback for "which bind is the scene", and `src/engines/phyre/scene_target.cpp` supplies Ayesha's. Core declines and says so in the log when no engine has registered one — which is the correct state for Escha & Logy and Shallie, whose renderer has never been censused.
- `src/engines/phyre/` — Ayesha (PhyreEngine, old MSVC CRT). Arland ports live here: the atlas cache, field physics, and the MSAA scene-target rule.
- `src/engines/ktgl/` — Escha & Logy and Shallie (LTGL/KTGL, UCRT). Fingerprinting plus one fix: the `Loadning`→`Loading` string correction (`loading_text_fix.cpp`), which patches a `.rdata` literal and hooks nothing. Nothing here detours anything yet.

  Plural on purpose. `src/core/engine.{h,cpp}` is the **dispatch layer** — it resolves which engine this process is and forwards to one module. `src/engines/` holds the modules it forwards to. Singular for the dispatcher, plural for the implementations.
- `src/launcher/` — both launcher pieces, neither of which is an engine module and neither of which shares code with the game DLL: `launcher_gui.cpp` is the 64-bit `dusk-fix-launcher.exe` settings window, and `launcher_proxy.cpp` is the 32-bit `msimg32.dll` the games' own front-ends load. They agree on ini key names and nothing else.

One `d3d11.dll` covers all three games. Do not split it per game or per engine: fixes are already gated on both the capability matrix and an executable fingerprint. See `WORK_DOC.md`, "Two engines, one DLL". `msimg32.dll` is a separate target only because the front-ends that load it are 32-bit processes.

Neither engine module may include the other's headers, and no address pack belongs in `src/core`.

User-facing options go in `dusk-fix.ini` through the capability matrix's `Descriptor`; environment switches are diagnostics and must not be given an ini key. See `WORK_DOC.md`, "Configuration: dusk-fix.ini".

This is the sibling of `../atelier-arland-fixes`. Read that repository's `AGENTS.md` and `TECHNICAL.md` first: its architecture (d3d11 proxy, capability matrix, hook idioms, documentation rules) is the template for this project.

## Documentation policy

This repository is still pre-release. `WORK_DOC.md` is its detailed technical and investigation record and may contain shipped measurements, work in progress, open questions and unvalidated ports. Keep it current as code and evidence change. The concise task queue is `../atelier-re-tools/DUSK.md`; do not create a second TODO or handoff file. `TECHNICAL.md` is the release-facing draft of finalized, measured behavior only; do not move open investigations or unvalidated ports into it. Refresh it as the first version approaches release.

## Game copies

Never modify or redistribute Koei Tecmo executables; launcher/game mutations stay in memory and signature-gated, following the Arland rules. Deploying means copying `build64/d3d11.dll` next to the game executable, plus `build64/dusk-fix-launcher.exe` and `build32/msimg32.dll` when the launcher is in scope.
