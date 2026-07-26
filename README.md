# Atelier Dusk Fixes

Performance and rendering fixes for the Steam releases of **Atelier Ayesha DX, Atelier Escha & Logy DX, and Atelier Shallie DX** (the Dusk trilogy).

This project is the Dusk-trilogy sibling of [atelier-arland-fixes](https://github.com/nicoverbruggen/atelier-arland-fixes). It is in early investigation: **nothing is released, and no fix ships yet.** What exists today is a D3D11 proxy and one opt-in diagnostic.

## Background

The Dusk DX ports share the Gust PSSG/KTGL engine family with the Arland DX ports, but differ in ways that matter for the fixes:

- Ayesha DX uses the old-MSVC-CRT/NLS runtime like the Arland games. Its font-atlas and text-rendering code path is the **same code**, function for function, as the one behind the Arland menu hitch — see `TECHNICAL.md` §1.
- Escha & Logy and Shallie are UCRT builds with fast menus. Their text-rendering layer has diverged and shares no homolog of that path, so the menu work does not apply to them.
- The upstream `atelier-sync-fix` targets the newer engine revisions directly; which games need which synchronization treatment is still open.

## Feature support by game

Mirrors the capability matrix in `src/game.cpp`, which is the source of truth.

| Feature | Ayesha | Escha & Logy | Shallie |
|---|---|---|---|
| Font-atlas diagnostic (`DUSK_ATLAS_STATS`) | opt-in | — | — |
| Font-atlas read cache | not implemented | — | — |

## The font-atlas diagnostic

Set `DUSK_ATLAS_STATS=1` and run Ayesha. The mod installs four verified hooks in counting mode only — nothing is cached, nothing is suppressed, every hook forwards straight to the original — and writes per-menu-build counters to `dusk-fix.log`.

This exists to answer one question before any fix is written: does Ayesha actually issue the redundant font-atlas reads that make Arland menus slow, and if so, on what lifetime? The addresses are mapped and corroborated; the behaviour is not yet measured. See `TECHNICAL.md` §1.8 for exactly what is and is not established.

## Documentation

- `TECHNICAL.md` — what has been established, with evidence, and the verified address packs.
- `BUILDING.md` — build and deploy.
- `TODO.md` — current work and backlog.

## Licence

MIT, with zlib-licensed portions derived from Philip Rebohle's `atelier-sync-fix`. See `LICENSE`.
