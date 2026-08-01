# AGENTS.md

## Project scope

Fixes for the Steam Dusk-trilogy DX ports: Atelier Ayesha DX, Atelier Escha & Logy DX, Atelier Shallie DX. This project is still pre-release; read `WORK_DOC.md` for the detailed current evidence and `../atelier-re-tools/DUSK.md` for the concise work queue.

## Source layout

Split by engine, because the trilogy spans two and they share nothing a fix can reach:

- `src/core/` — engine-agnostic: the D3D11 proxy (`main.cpp`), engine dispatch (`engine.cpp`), the capability matrix (`game.cpp`), hook installation, logging.
- `src/phyre/` — Ayesha (PhyreEngine, old MSVC CRT). Arland ports live here: the atlas cache, field physics.
- `src/ktgl/` — Escha & Logy and Shallie (LTGL/KTGL, UCRT). Fingerprinting only so far.

One `d3d11.dll` covers all three games. Do not add a second build target: fixes are already gated on both the capability matrix and an executable fingerprint. See `WORK_DOC.md`, "Two engines, one DLL".

Neither engine module may include the other's headers, and no address pack belongs in `src/core`.

This is the sibling of `../atelier-arland-fixes`. Read that repository's `AGENTS.md` and `TECHNICAL.md` first: its architecture (d3d11 proxy, capability matrix, hook idioms, documentation rules) is the template for this project.

## Documentation policy

This repository is still pre-release. `WORK_DOC.md` is its detailed technical and investigation record and may contain shipped measurements, work in progress, open questions and unvalidated ports. Keep it current as code and evidence change. The concise task queue is `../atelier-re-tools/DUSK.md`; do not create a second TODO or handoff file. There is no `TECHNICAL.md` yet. Create one from finalized, measured behaviour only when the first version of the mod is ready.

## Game copies

Never modify or redistribute Koei Tecmo executables; launcher/game mutations stay in memory and signature-gated, following the Arland rules. Deploying means copying `build64/d3d11.dll` next to the game executable.
