# Atelier Dusk Fixes

Performance and rendering fixes for the Steam releases of **Atelier Ayesha DX, Atelier Escha & Logy DX, and Atelier Shallie DX** (the Dusk trilogy).

This project is the Dusk-trilogy sibling of [atelier-arland-fixes](https://github.com/nicoverbruggen/atelier-arland-fixes). It is early: one fix ships (much faster Ayesha menus), and everything else here is opt-in and unfinished.

## Background

The Dusk DX ports share the Gust PSSG/KTGL engine family with the Arland DX ports, but differ in ways that matter for the fixes:

- Ayesha DX uses the old-MSVC-CRT/NLS runtime like the Arland games. Its font-atlas and text-rendering code path is the **same code**, function for function, as the one behind the Arland menu hitch — see `WORK_DOC.md`, "The engine triage".
- Escha & Logy and Shallie are UCRT builds with fast menus. Their text-rendering layer has diverged and shares no homolog of that path, so the menu work does not apply to them.
- The upstream `atelier-sync-fix` targets the newer engine revisions directly; which games need which synchronization treatment is still open.

## Feature support by game

Mirrors the capability matrix in `src/core/game.cpp`, which is the source of truth.

| Feature | Ayesha | Escha & Logy | Shallie |
|---|---|---|---|
| Font-atlas diagnostic (`DUSK_ATLAS_STATS`) | opt-in | — | — |
| Font-atlas sequence trace (`DUSK_ATLAS_TRACE`) | opt-in | — | — |
| Font-atlas cache verifier (`DUSK_ATLAS_VERIFY`) | opt-in | — | — |
| Render-target census (`DUSK_TARGET_CENSUS`) | opt-in | opt-in | opt-in |
| Render above 1080p properly (`[Rendering] HighResolution`) | opt-in | — | — |
| Much faster menus (font-atlas read cache) | ✓ | — | — |
| Experimental field-jitter threshold rescale (`DUSK_FIELD_ENGINE_FIX`) | opt-in | — | — |
| Experimental field-jitter resting stabilizer (`DUSK_FIELD_STABILIZER`) | opt-in | — | — |

## Much faster menus (Ayesha)

Ayesha's menus are slow to open because the game re-reads its three 512×512 font atlases thousands of times while building a menu, and again every frame while one is on screen. The mod serves those repeated reads from a CPU snapshot with a frame-scoped lifetime, so each atlas is read at most once per frame instead of hundreds of times.

Measured on the English build: menu construction fell from **248 ms to 38 ms**, with 95.5% of atlas reads served from cache. Nothing to configure — it is on by default. `DUSK_ATLAS_CACHE=0` turns it off, which is what a comparison or a bug report wants.

Ayesha only. Escha & Logy and Shallie do not share this code path and are unaffected.

## High-refresh field jitter

This is an experimental port of Arland's fix for vertical field-map jitter above roughly 115 fps. All three Ayesha anchors are resolved and the constant has exactly one reader in the image, but the current implementation was tested in game and **did not fix the problem**. The Ayesha-specific mechanism and live controller layout require further research; neither switch is a validated fix.

`DUSK_FIELD_ENGINE_FIX=1` rescales the constant with frame time. `DUSK_FIELD_STABILIZER=1` additionally holds the character while it is genuinely at rest, and needs the rescale.

Run `DUSK_FIELD_TRACE=1` before enabling the stabilizer. The stabilizer writes into the live controller object at offsets carried over from the Arland builds, and Ayesha's object layout is not confirmed; the trace reads those same offsets, so it tells you cheaply whether they hold.

## The diagnostic

Set `DUSK_ATLAS_STATS=1` and run Ayesha. The mod installs four verified hooks in counting mode only — nothing is cached, nothing is suppressed, every hook forwards straight to the original — and writes per-menu-build counters to `dusk-fix.log`.

It answered the question the fix needed: Ayesha issues 2385 candidate atlas locks against just 3 atlases per 248 ms menu drain, and 72% of all such locks fall outside the resource queue drain — which is why the cache is frame-scoped. See `WORK_DOC.md`, "Repeated font-atlas reads".

`DUSK_ATLAS_VERIFY=1` is the cache's correctness check. It compares each snapshot against the real atlas for as long as that snapshot is supposed to match, and separately reports any write to an atlas that the cache did not serve — the two ways a stale glyph could reach the screen. It makes the game slow and is meant to be run, not shipped. It reports a running tally every few hundred frames regardless of the other switches: a clean session is one where `checks` is large and `mismatches` and `foreignWrites` are both zero, and a session where `checks` never leaves zero has proved nothing. It exists because a wrong glyph in Japanese is not something a reader can reliably spot, so the check has to be machine-made.

Adding `DUSK_ATLAS_TRACE=1` alongside the diagnostic dumps the raw lock/unlock sequence of a single steady-state frame as a token stream. It is useful for diagnosing unexpected cache churn or invalidation; a normal steady-state frame reaches the read-side floor of three real reads total, one per atlas, while the remaining real locks are write mappings. It prints once, then stops. See `WORK_DOC.md`, "Diagnostics".

## Settings

Options you are meant to change live in `dusk-fix.ini`, created beside the game the first time the mod runs. Environment variables such as `DUSK_ATLAS_STATS` are diagnostics: they are meant for producing a log to attach to a report, they are slow on purpose, and they deliberately have no ini key.

| Key | What it does |
|---|---|
| `[Launcher] SkipLauncher` | start the game directly, skipping both of Koei Tecmo's front-ends |
| `[Launcher] AutoResolution` | remembers that you picked Auto, so the launcher keeps following your desktop |
| `[Rendering] HighResolution` | render the scene at the chosen resolution instead of 1080p |
| `[Fixes] AtlasCache` | the font-atlas read cache — the much-faster-menus fix, on by default |
| `[Fixes] FieldEngineFix` | experimental field-jitter threshold rescale |
| `[Fixes] FieldStabilizer` | experimental field-jitter resting stabilizer |

Keys appear in the file for the game they apply to, so an Escha & Logy or Shallie install gets only `SkipLauncher`. That is accurate rather than incomplete: the rest are Ayesha-only.

## The launcher

`dusk-fix-launcher.exe` puts the game's own settings and the mod's in one window and starts the game from it: resolution, window mode, language and character outlines on the game's side, and the mod's switches on the other. Koei Tecmo's own launcher and settings editor both stay reachable from it, and so is starting the game with the mod stood down.

The resolution list leads with **Auto**, which follows your desktop resolution and is what a fresh install gets. The list is not filtered through Windows' display-mode reporting, which is what can hide a perfectly usable mode on a handheld or in docked use, and whatever the game's file already holds is always offered too — so opening the window never silently changes a resolution it did not offer.

`msimg32.dll` is a second, 32-bit DLL that the game's own launcher loads, and is what opens the above in place of the stock launcher when the game is started from Steam. Both files are optional: with neither installed, or with only one of them, the stock launcher comes up exactly as before. `[Launcher] SkipLauncher=true` skips every launcher and starts the game directly.

Nothing about either of Koei Tecmo's front-end programs is modified on disk. The redirect rewrites five bytes at the launcher's entry point in memory and puts them back if the launch cannot proceed.

## Resolution

Ayesha reads its resolution from `[Graphics] ScreenWidth`/`ScreenHeight` in its own `Setting.ini` and accepts any value you put there, so the mod does not add a resolution setting of its own.

On its own, though, selecting a higher resolution does **not** get you a sharper picture. Measured at 1440p: the game's window and its depth buffer are 1440p, but everything the scene is drawn into stays at 1920×1080, so the scene is rendered at 1080p and scaled up.

**Tick "Render at the selected resolution"** in the launcher (`[Rendering] HighResolution`) and the scene targets follow the resolution instead, along with the viewport and scissor the game hard-codes to match them. It is off by default for now because it has not been played through yet; the failure mode would be a visibly wrong picture rather than a quiet regression, so it is worth one round of eyes first.

This is the same defect the Arland mod fixes in those games, and the mechanism is TellowKrinkle's. See `WORK_DOC.md`, "The high-resolution fix".

## Source layout

The trilogy spans two engines, and the source follows that split: `src/core` is engine-agnostic, `src/phyre` is Ayesha (PhyreEngine), `src/ktgl` is Escha & Logy and Shallie (LTGL/KTGL). They still build into one `d3d11.dll` that covers all three games — every fix is gated on both the capability matrix and an executable fingerprint, so the module for the wrong engine installs nothing. `WORK_DOC.md`, "Two engines, one DLL", has the reasoning.

`src/ktgl` implements no fix yet. It verifies and logs the four known executable identities, providing the gate future LTGL/KTGL fixes will install behind.

`src/launcher` holds both launcher pieces — the 64-bit launcher window and the 32-bit `msimg32.dll` proxy. Neither shares code with the game DLL.

## Documentation

- `WORK_DOC.md` — the pre-release technical and investigation record, including measurements, open questions and verified address packs.
- `BUILDING.md` — build and deploy.

## Licence

MIT, with zlib-licensed portions derived from Philip Rebohle's `atelier-sync-fix`. See `LICENSE`.
