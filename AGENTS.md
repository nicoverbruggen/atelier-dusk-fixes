# AGENTS.md

## Project scope

Fixes for the Steam Dusk-trilogy DX ports: Atelier Ayesha DX, Atelier Escha & Logy DX, Atelier Shallie DX. Early investigation stage — no releases, no shipped code yet.

This is the sibling of `../atelier-arland-fixes`. Read that repository's `AGENTS.md`, `TECHNICAL.md`, and `TODO.md` first: its architecture (d3d11 proxy, capability matrix, hook idioms, documentation rules) is the template for this project, and its "Beyond the Arland trilogy" TODO section carries the Dusk-related decisions made so far (e.g. Ayesha gets an atlas-only fix without the Arland `.PSSG` cache).

## Game copies

Never modify or redistribute Koei Tecmo executables; launcher/game mutations stay in memory and signature-gated, following the Arland rules. Deploying means copying `build64/d3d11.dll` next to the game executable.

## Known starting facts

- Ayesha DX: old-MSVC-CRT build; suffers the English menu-hitch class the Arland project solved. The Arland `d3d11.dll` deliberately does not patch it; an atlas-only path was planned (`ARLAND` TODO).
- Escha & Logy DX / Shallie DX: UCRT builds, fast menus. Yuri Hime's Atelier Graphics Tweak targeted these titles (SMAA, resolution hack, the withdrawn anti-stutter, an Escha & Logy shadow-texture fix, a Shallie sampler-state bug patch) — the archived AGT DLLs are kept locally for reference.
- Steam appids: Ayesha 1152300 (verify), Escha & Logy 1152310, Shallie 1152320.
