# Changelog

## v0.1 (unreleased)

### Added

- **Ayesha font-atlas read caching.** Repeated reads of the three 512x512 font atlases are served from frame-scoped CPU snapshots while the verified text renderer is active. A representative 248.4 ms menu build fell to 38.2 ms, with 95.5% of atlas reads served from cache. The cache is on by default for Ayesha and hard-off for Escha & Logy and Shallie.
- **Ayesha high-resolution scene rendering.** Render and depth targets that the engine pins to 1920x1080 now follow a selected resolution above 1080p. The matching blur target, viewport, and scissor correction are included. The behavior was confirmed in game at 3840x2160 and costs nothing at or below 1080p.
- **Ayesha high-refresh field movement correction.** The fixed movement threshold is rescaled with frame time and a coupled resting stabilizer prevents the character's vertical sawtooth while grounded. The defect was measured at 12-18 pixels of vertical excursion while standing still, and the correction was confirmed in game with both halves enabled.
- **A Dusk-specific launcher.** The 64-bit launcher window, 32-bit `msimg32.dll` redirect, `SkipLauncher` path, language selection, stock-tool buttons, and `Setting.ini`/`dusk-fix.ini` configuration layer are implemented. The current tabbed build still requires the complete validation session listed in `TECHNICAL.md` before this entry can be released.
- **"Loadning system data." corrected in Escha & Logy and Shallie.** The English status line on the games' first screen misspells "Loading". The string is a plain literal in each of the four executables, so the mod verifies and rewrites those 22 bytes in the loaded image at startup, restoring the page's read-only protection afterwards. No hook, no per-frame cost, and nothing written to disk. On by default; `DUSK_LOADING_TEXT=0` leaves it alone. The addresses and bytes are verified statically in all four builds; the corrected line has not yet been read off a running game.
- **Runtime diagnostics.** Ayesha atlas statistics, lock traces, cache verification, writer census, D3D11 write probes, field traces, and all-game render-target census support measured validation and bug reports.

### Scope

- Escha & Logy and Shallie are recognized and logged, and receive the "Loadning" text correction, but their LTGL/KTGL engine has no enabled gameplay or rendering fixes yet.
- The first public release is not cut by this repository state. The launcher validation and remaining release bookkeeping must be completed first.

## Earlier development

Finalized, measured behaviour for each entry above is described in [TECHNICAL.md](TECHNICAL.md). Open work is tracked privately and is not published here.
