# AGENTS.md

## Project scope

Fixes for the Steam Dusk-trilogy DX ports: Atelier Ayesha DX, Atelier Escha & Logy DX, Atelier Shallie DX. Early stage — no releases; one fix ships (the Ayesha font-atlas cache), everything else is opt-in and unvalidated.

## Source layout

Split by engine, because the trilogy spans two and they share nothing a fix can reach:

- `src/core/` — engine-agnostic: the D3D11 proxy (`main.cpp`), engine dispatch (`engine.cpp`), the capability matrix (`game.cpp`), hook installation, logging.
- `src/phyre/` — Ayesha (PhyreEngine, old MSVC CRT). Arland ports live here: the atlas cache, field physics.
- `src/ktgl/` — Escha & Logy and Shallie (LTGL/KTGL, UCRT). Fingerprinting only so far.

One `d3d11.dll` covers all three games. Do not add a second build target: fixes are already gated on both the capability matrix and an executable fingerprint. See `TECHNICAL.md`, "Two engines, one DLL".

Neither engine module may include the other's headers, and no address pack belongs in `src/core`.

This is the sibling of `../atelier-arland-fixes`. Read that repository's `AGENTS.md` and `TECHNICAL.md` first: its architecture (d3d11 proxy, capability matrix, hook idioms, documentation rules) is the template for this project.

## Documentation policy

`TECHNICAL.md` records **shipped, measured behaviour only** — the same policy as the Arland repository. Work in progress, open questions, unvalidated ports and investigation narrative do not go in it, and neither repository carries a `TODO.md`; the task lists are maintained outside these repositories. If a fix is not finished, it does not get a `TECHNICAL.md` section, however interesting the investigation was.

## Game copies

Never modify or redistribute Koei Tecmo executables; launcher/game mutations stay in memory and signature-gated, following the Arland rules. Deploying means copying `build64/d3d11.dll` next to the game executable.

## Known starting facts

- Ayesha DX: old-MSVC-CRT build; suffers the English menu-hitch class the Arland project solved. The Arland `d3d11.dll` deliberately does not patch it; an atlas-only path was planned (`ARLAND` TODO).
- Escha & Logy DX / Shallie DX: UCRT builds, fast menus. Yuri Hime's Atelier Graphics Tweak targeted these titles (SMAA, resolution hack, the withdrawn anti-stutter, an Escha & Logy shadow-texture fix, a Shallie sampler-state bug patch) — the archived AGT DLLs are kept locally for reference.
- Steam appids: Ayesha 1152300 (verify), Escha & Logy 1152310, Shallie 1152320.
