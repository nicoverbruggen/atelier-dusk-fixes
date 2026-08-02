# Technical overview

This document records finalized, measured behavior for the first planned Dusk release. Ongoing reverse engineering, unvalidated ports, and open questions remain in [WORK_DOC.md](WORK_DOC.md) and are not release guarantees.

## Scope and engines

One 64-bit `d3d11.dll` recognizes all six Dusk game executables. Ayesha is the PhyreEngine-derived target and uses the atlas and field modules under `src/phyre`. Escha & Logy and Shallie use LTGL/KTGL and are currently fingerprinted for identification only under `src/ktgl`.

The capability matrix in `src/core/game.cpp` is the source of truth. Ayesha has the atlas cache, high-resolution correction, and field correction enabled by default. Target census is available as a diagnostic for all three games. Every engine-specific feature is hard-off for an unsupported title.

## Font-atlas cache

Ayesha builds menus by repeatedly mapping the same three 512x512 font atlases. A representative unmodified menu drain made 2,385 candidate locks and took 248.4 ms. The cache reduced that drain to 38.2 ms, an 85% reduction. In the measured session it served 63,517 cache hits against 3,029 real reads, a 95.5% hit rate.

The cache is frame-scoped because 72% of candidate locks occurred outside the resource queue drain. It is eligible only from the verified text-renderer call path and for 512x512 atlas textures. Snapshots are created only from read locks, while existing snapshots can serve both read and write modes. An unmatched real unlock invalidates that texture's snapshot, and a partial hook installation remains pass-through rather than enabling half the cache.

The English and multilingual Ayesha executables use separate address packs. The four required anchors are the queue drain, text renderer, atlas lock, and atlas unlock stub. The cache is hard-off for Escha & Logy and Shallie because their text-rendering layer has no verified equivalent.

`DUSK_ATLAS_STATS`, `DUSK_ATLAS_TRACE`, `DUSK_ATLAS_VERIFY`, `DUSK_ATLAS_CENSUS`, and `DUSK_D3D11_WRITE_PROBE` are diagnostic paths only. Their behavior and interpretation are documented in [ADVANCED.md](ADVANCED.md).

## High-resolution rendering

Ayesha accepts a high display resolution but leaves several internal scene targets at 1920x1080. The correction uses a narrow `CreateTexture2D` classification:

- The first suitable depth target becomes the main render size only when its shape matches the swap chain or is a valid 16:9 target at least 1920x1080.
- Empty 1920x1080 render and depth targets are resized to the main render size.
- The exact 960x540 typeless BGRA blur target is resized to half-size.
- Existing data, unrelated textures, smaller pyramid levels, and the 1024x1024 shadow map pass through.
- If an enlarged target is rejected, creation is retried with the game's original descriptor.

Viewport and scissor correction is deferred until draw time, when the bound target can be checked. Immediate and deferred context vtables are hooked separately because Ayesha submits its draws through a deferred context. The correction was confirmed at 3840x2160: the main render size followed the swap chain, and the in-game frame was sharper and correctly framed. At 1080p, the hooks remain passive because no target is larger than the pinned size.

`DUSK_HIGHRES=0` disables the correction for one session. It has no ini key because selecting the output resolution is already the user decision the correction implements.

## High-refresh field movement

Ayesha's field controller discarded sub-threshold movement using a distance that was correct only at the shipped frame cadence. A 144 Hz baseline measured 12-18 pixels of vertical character excursion while the character was horizontally at rest on the atelier steps. The motion had the sustained sawtooth shape expected from gravity accumulating against the collision threshold.

The correction has two coupled parts. The engine threshold is rescaled with frame time, and a stabilizer holds a genuinely resting grounded controller while pinning the air timer. The stabilizer refuses to install without the rescale. Both Ayesha executable builds have independently checked controller and collision anchors and the correction was confirmed in game with both halves active.

`DUSK_FIELD_ENGINE_FIX=0` disables the complete correction for one session. `DUSK_FIELD_STABILIZER=0` disables only the second half for comparison.

## Launcher

The custom launcher is a 64-bit Win32 settings program. It edits the game's `Setting.ini` for resolution, fullscreen mode, language, and outlines, and `dusk-fix.ini` for launcher state. Auto is represented in `dusk-fix.ini` and resolved to a literal desktop resolution in `Setting.ini` when saved.

The 32-bit `msimg32.dll` proxy is loaded by each game's stock launcher and settings editor. It forwards `AlphaBlend` and `TransparentBlt`. Only the three per-game stock launcher processes are redirected, and only when the custom launcher is installed or `SkipLauncher` selects a game executable. The proxy patches the host entry point in memory, preserves the original bytes for fallback, and keeps the Steam-launched process alive while its child runs.

The launcher implementation is not yet release-validated. The required validation covers layout and DPI scaling, save round-trips, Auto, language-based game selection, stock-tool buttons, all three titles, and `SkipLauncher`.

## Runtime safety

The mod never modifies the game files. Game hooks are gated on executable identity, `.text` size, and complete expected prologues before MinHook is called. Unknown builds receive only normal D3D11 forwarding. Reverse-engineered memory access uses guarded range checks, and all patches, trampolines, snapshots, and cached pointers disappear with the process.

## Provenance

Philip Rebohle created the original `atelier-sync-fix` synchronization implementation. TellowKrinkle's fork supplied prior Ayesha Map/Unmap work and the old-Arland rendering correction. The Dusk atlas and field measurements, address mapping, and integration are this project's work. Atelier Graphics Tweak is prior behavioural evidence only; none of its code is included. MinHook is by Tsuda Kageyu and contributors.
