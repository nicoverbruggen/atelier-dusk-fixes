# Changelog

## v0.1 (unreleased)

### Added

- **Ayesha font-atlas read caching.** Repeated reads of the three 512x512 font atlases are served from frame-scoped CPU snapshots while the verified text renderer is active. A representative 248.4 ms menu build fell to 38.2 ms, with 95.5% of atlas reads served from cache. The cache is on by default for Ayesha and hard-off for Escha & Logy and Shallie.
- **Ayesha high-resolution scene rendering.** Render and depth targets that the engine pins to 1920x1080 now follow a selected resolution above 1080p. The matching blur target, viewport, and scissor correction are included. The behavior was confirmed in game at 3840x2160 and costs nothing at or below 1080p.
- **Ayesha high-refresh field movement correction.** The fixed movement threshold is rescaled with frame time and a coupled resting stabilizer prevents the character's vertical sawtooth while grounded. The defect was measured at 12-18 pixels of vertical excursion while standing still, and the correction was confirmed in game with both halves enabled.
- **Ayesha travel-map cursor correction.** The cursor's step was added to its position with no frame-time term, so it crossed the world map more than three times too fast at 200 Hz. The produced step is now rescaled, which preserves the shipped behavior exactly at 60 fps and below. Confirmed in game.
- **Ayesha antialiasing and supersampling.** SMAA runs before the interface is composited, so it smooths the scene without softening menu text. Supersampling renders the scene above the display size and resamples it with a box filter sized to the ratio, sharpening folded into the same pass; fractional multipliers are usable because the resample is the mod's own. Both confirmed in game at 2560x1440. Supersampling is opt-in.
- **Synthesis animation cadence corrected in Escha & Logy and Shallie.** The synthesis product cards run off a fixed-timestep pump whose loop is bottom-tested, so it ticked at least once per rendered frame: 3.34 times too fast at 200 Hz. The correction skips the tick when the accumulator has not reached one step, using the engine's own constants, so at 59.94 Hz and below the original runs untouched. Confirmed in game in both titles.
- **System-save protection for Escha & Logy and Shallie.** A failed load of `SYSDATA.pcsave` reported success, installed a zero-filled buffer over the live settings, and the next settings change wrote defaults back over the file. The mod forces the engine's own read-failure state after a load that claims completion having read nothing, and refuses system-data saves until a load genuinely succeeds. The defect was reproduced and the fix confirmed in game.
- **Shallie control-hint panel hold.** The panel replayed its slide-in entrance whenever the containing interface was rebuilt. The correction hands the game's own easing code a larger frame time so each pane lands in one frame using the game's own limits. Opt-in, since suppressing a shipped interface behavior is a preference.
- **"Loadning system data." corrected in Escha & Logy and Shallie.** The English status line on the games' first screen misspells "Loading". The string is a plain literal in each of the four executables, so the mod verifies and rewrites those 22 bytes in the loaded image at startup, restoring the page's read-only protection afterwards. No hook, no per-frame cost, and nothing written to disk. On by default; `DUSK_LOADING_TEXT=0` leaves it alone.
- **Ayesha startup logo and opening movie skips.** Both opt-in, both using the mechanism the Arland mod uses, since Ayesha runs the same PhyreEngine boot code. Skipping the logos does not start the game sooner: they play while the game loads, so a black screen replaces them for as long as loading takes. The movie skip gates on the movie's index, so the endings and event movies are untouched.
- **A black startup screen instead of a grey one on Ayesha.** The game's window class carries the grey stock brush, so Windows fills the window with mid-grey for about a second before the first frame. Black is what the game fades up from. One field of one window class, substituted only when the class is the engine's own and its brush is exactly the grey stock object.
- **A Dusk-specific launcher.** The 64-bit launcher window, 32-bit `msimg32.dll` redirect, `SkipLauncher` path, language selection, stock-tool buttons, and `Setting.ini`/`dusk-fix.ini` configuration layer.
- **Runtime diagnostics.** Ayesha atlas statistics, lock traces, cache verification, writer census, D3D11 write probes, field traces, and all-game render-target census support measured validation and bug reports.

### Changed

- **The launcher now matches the Arland mod's, tab for tab and row for row.** Three tabs (General, Graphics, About) in the same order with the same wording, the stock-tool buttons on About, and the skip-launcher checkbox on General. A setting the running game does not have is no longer shown at all: the window reads the same per-game capability list the DLL does, so Escha & Logy and Shallie show neither supersampling nor edge smoothing, and saving never writes a key for a feature the running game would ignore.
- **The launcher reports a failed save instead of losing it silently.** Every write is checked, and a reported failure is verified against the file before it becomes a warning, because the flush call reports failure under Wine even when the values reached disk. Real failures name the Win32 reason and go to `dusk-fix.log`. Resetting to defaults now asks first and saves; a missing `dusk-fix.ini` is created rather than refused; and the launcher stops with an explanation when it is not beside a game.
- **The image-quality preset ladder was removed.** With multisampling gone it set two adjacent controls, one of which already writes the resulting resolution into its own labels, so it said nothing the controls underneath did not.

### Removed

- **Multisample antialiasing.** It cannot reach what actually aliases in these games, which is detail inside textures and along alpha-tested edges; only supersampling resolves that. The engine's own multisampled targets are never rendered into, so there was no engine setting to turn up either, and the mod's twin implementation carried the whole cost of substituting and resolving a second set of render targets. The `[Rendering] MSAA` key, the `DUSK_MSAA` switch and the launcher control are gone with it. The Arland mod removed its own multisampling in the same week and for the same reasons.

### Scope

- The startup skips and the black startup screen are Atelier Ayesha only. The two KTGL games run a different boot path, and neither the logo object, the movie routine nor the window class name has a counterpart there.
- Escha & Logy and Shallie receive the loading-text correction, the system-save guard, the synthesis-animation correction and, on Shallie, the control-hint hold. They have no rendering fixes: their LTGL/KTGL renderer has not been censused, so nothing knows which part of a frame carries their 3D scene, and every image-quality feature needs that answer first.
- The first public release is not cut by this repository state. The remaining release bookkeeping must be completed first.

## Earlier development

Finalized, measured behaviour for each entry above is described in [TECHNICAL.md](TECHNICAL.md). Open work is tracked privately and is not published here.
