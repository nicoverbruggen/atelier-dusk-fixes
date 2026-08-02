# Advanced configuration

The normal user-facing settings are in `dusk-fix.ini`, and the custom launcher is the intended way to edit them. This document covers the options the launcher cannot explain, one-session A/B switches, diagnostics, and troubleshooting. For installation and the default feature set, see [README.md](README.md).

## Configuration files

`dusk-fix.ini` is beside `d3d11.dll`. It is created on first launch with `[Launcher] SkipLauncher=false` when no file exists. Feature values are resolved in this order: environment variable, ini value where one exists, then the per-game capability matrix. Unsupported features are always off.

The game's own `Setting.ini` remains separate. Resolution, fullscreen mode, language, and character outlines belong there because the game reads those values itself. The custom launcher writes both files without removing unrelated keys.

### `[Launcher]`

| Key | Values | Default | Effect |
|---|---|---|---|
| `SkipLauncher` | `true` / `false` | `false` | Starts the selected game directly from Steam without showing either launcher. |
| `AutoResolution` | `true` / `false` | `false` | Remembers that Auto was selected. The launcher resolves Auto to the current desktop mode when saving it to `Setting.ini`. |

`SkipLauncher=true` is useful after the game settings are configured. The game is still started as a child of the Steam-launched process, so the overlay and Steam Input remain attached. Run `dusk-fix-launcher.exe` directly to change settings or turn this option off.

`DUSK_NO_REDIRECT=1` temporarily opens Koei Tecmo's original launcher instead of the custom one. The custom launcher's buttons set this only for the child process so they cannot redirect back to themselves.

## One-session A/B switches

These are environment variables, not ini keys. They are for comparison and bug reports and should not be exported permanently by wrapper scripts.

| Variable | Effect |
|---|---|
| `DUSK_DISABLE=1` | Stands the whole mod down while continuing to forward D3D11. The game runs as shipped. |
| `DUSK_ATLAS_CACHE=0` | Disables Ayesha's font-atlas cache. |
| `DUSK_HIGHRES=0` | Disables Ayesha's high-resolution render-target correction. |
| `DUSK_FIELD_ENGINE_FIX=0` | Disables the Ayesha field-jitter fix. The resting stabilizer depends on this rescale. |
| `DUSK_FIELD_STABILIZER=0` | Disables only the resting part of the field-jitter fix. This leaves the threshold rescale active. |
| `DUSK_LOADING_TEXT=0` | Leaves the "Loadning system data." misspelling on Escha & Logy's and Shallie's first screen uncorrected. |

Environment values are read before the ini layer. A wrapper that exports one of these variables, even with a default value, overrides the file and can make a validation run test the wrong configuration.

## Diagnostics

Diagnostics write to `dusk-fix.log` beside the game. A useful report includes the log, the exact executable name, the selected resolution, and the environment variables used for the run.

### Ayesha font-atlas diagnostics

| Variable | Effect |
|---|---|
| `DUSK_ATLAS_STATS=1` | Counts atlas locks, reads, writes, render-text calls, timings, cache hits, and snapshot churn without changing the atlas behavior. |
| `DUSK_ATLAS_TRACE=1` | With statistics enabled, dumps one warmed steady-state frame's lock/unlock token sequence. |
| `DUSK_ATLAS_VERIFY=1` | With the cache enabled, compares snapshots against the real atlas and reports mismatches or foreign writes. It is intentionally slow. |
| `DUSK_ATLAS_CENSUS=1` | Enumerates atlas lock callers, access modes, threads, and textures. |
| `DUSK_D3D11_WRITE_PROBE=1` | Probes D3D11-level writes to candidate atlas textures. |
| `DUSK_ATLAS_CACHE=0` | A/B control for the shipping cache; see above. |

A useful cache-verification session has a large `checks` count and zero `mismatches` and `foreignWrites`. A zero count proves that the verification path did not run and is not evidence of correctness.

### Rendering and field diagnostics

| Variable | Effect |
|---|---|
| `DUSK_TARGET_CENSUS=1` | Logs the render and depth target shapes and the high-resolution action taken for each target. Works in all three games. |
| `DUSK_FIELD_TRACE=1` | Logs controller state around Ayesha ground-contact changes. Quiet standing produces no output. |
| `DUSK_UI_SCALE=100..200` | Enlarges the custom launcher UI for DPI or TV-distance testing. It is read by the launcher, not the game DLL. |

The target census is the diagnostic to use when investigating Escha & Logy or Shallie rendering. It does not enable a fix in those games.

## Logs and failure modes

The game DLL logs the version, title, engine, forwarding route, resolved configuration, and feature installation status. A feature that fails its fingerprint or prologue check should report `failed` or remain inactive rather than partially patching an unknown build.

The launcher proxy is silent in normal builds. A diagnostic build compiled with `DUSK_LAUNCHER_DIAGNOSTIC` writes `dusk-launcher.log` beside the launcher process and records forwarding and redirect decisions.

If the mod does not load under Proton, verify `WINEDLLOVERRIDES="d3d11=n,b" %command%` and look for `dusk-fix.log`. If a visual or stability problem appears, first repeat it with `DUSK_DISABLE=1`, then compare with the relevant feature switch set to `0`.
