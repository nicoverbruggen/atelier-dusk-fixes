# Atelier Dusk Fixes

Performance and rendering fixes for the Steam releases of **Atelier Ayesha DX, Atelier Escha & Logy DX, and Atelier Shallie DX** (the Dusk trilogy).

This project is the Dusk-trilogy sibling of [atelier-arland-fixes](https://github.com/nicoverbruggen/atelier-arland-fixes). It is early: one fix ships (much faster Ayesha menus), and everything else here is opt-in and unfinished.

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
| Much faster menus (font-atlas read cache) | ✓ | — | — |
| High-refresh field jitter, threshold rescale (`DUSK_FIELD_ENGINE_FIX`) | opt-in | — | — |
| High-refresh field jitter, resting stabilizer (`DUSK_FIELD_STABILIZER`) | opt-in | — | — |

## Much faster menus (Ayesha)

Ayesha's menus are slow to open because the game re-reads its three 512×512 font atlases thousands of times while building a menu, and again every frame while one is on screen. The mod serves those repeated reads from a CPU snapshot with a frame-scoped lifetime, so each atlas is read at most once per frame instead of hundreds of times.

Measured on the English build: menu construction fell from **248 ms to 38 ms**, with 95.5% of atlas reads served from cache. Nothing to configure — it is on by default. `DUSK_ATLAS_CACHE=0` turns it off, which is what a comparison or a bug report wants.

Ayesha only. Escha & Logy and Shallie do not share this code path and are unaffected.

## High-refresh field jitter

Ported from Arland, where above roughly 115 fps the field-map character buzzes vertically while standing on a step or ledge, because a collision-resolver constant means a per-frame *distance* that was only ever right at 60 fps. All three Ayesha anchors are resolved and the constant has exactly one reader in the image, but **whether Ayesha actually shows the symptom has not been measured.**

`DUSK_FIELD_ENGINE_FIX=1` rescales the constant with frame time. `DUSK_FIELD_STABILIZER=1` additionally holds the character while it is genuinely at rest, and needs the rescale.

Run `DUSK_FIELD_TRACE=1` before enabling the stabilizer. The stabilizer writes into the live controller object at offsets carried over from the Arland builds, and Ayesha's object layout is not confirmed; the trace reads those same offsets, so it tells you cheaply whether they hold. See `TECHNICAL.md` §5.

## The diagnostic

Set `DUSK_ATLAS_STATS=1` and run Ayesha. The mod installs four verified hooks in counting mode only — nothing is cached, nothing is suppressed, every hook forwards straight to the original — and writes per-menu-build counters to `dusk-fix.log`.

It answered the question the fix needed: Ayesha issues 2385 candidate atlas locks against just 3 atlases per 248 ms menu drain, and 72% of all such locks fall outside the resource queue drain — which is why the cache is frame-scoped. See `TECHNICAL.md` §2.

## Documentation

- `TECHNICAL.md` — what has been established, with evidence, and the verified address packs.
- `BUILDING.md` — build and deploy.
- `TODO.md` — current work and backlog.

## Licence

MIT, with zlib-licensed portions derived from Philip Rebohle's `atelier-sync-fix`. See `LICENSE`.
