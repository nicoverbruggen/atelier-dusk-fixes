# Atelier Dusk Fixes

Performance and rendering fixes for the Steam releases of **Atelier Ayesha DX, Atelier Escha & Logy DX, and Atelier Shallie DX**, the Dusk trilogy.

> [!IMPORTANT]
> **Pre-release:** This repository is being prepared for its first `v0.1` release. Ayesha's menu, high-resolution rendering, and high-refresh field fixes are implemented and measured, but the launcher still needs a complete validation session before a public release. Escha & Logy and Shallie currently receive no engine-specific fixes.

The project ships one 64-bit `d3d11.dll` for all three games, an optional 64-bit `dusk-fix-launcher.exe`, and an optional 32-bit `msimg32.dll` proxy for the games' front-ends. The game DLL and launcher are separate processes and separate targets. Nothing modifies the original game executables on disk.

## What is supported

The table below describes the current source state. A check mark means the behavior has been measured or confirmed as described in `TECHNICAL.md`.

### Fixes

| Feature | Ayesha | Escha & Logy | Shallie |
|---|:---:|:---:|:---:|
| Much faster font-atlas reads | ✓ | — | — |
| Render scene targets above 1080p correctly | ✓ | — | — |
| Steady field movement at high refresh rates | ✓ | — | — |
| SMAA antialiasing (`[Rendering] SMAA`) | opt-in | — | — |
| MSAA sample count (`[Rendering] MSAA`) | opt-in | — | — |

SMAA is the one entry above that is off by default, and the reason is where it currently runs rather than what it does. These games ship no antialiasing at all; SMAA (Jimenez et al.) works on the finished image, so it smooths every visible edge rather than only polygon silhouettes as MSAA does. The passes are ported unchanged from the Arland mod, but that project injects them on the scene target *before* the game composites its interface, so menus and text stay crisp. The equivalent boundary in Ayesha's renderer has not been found yet, so for now the passes run at Present over the whole finished frame, which antialiases the UI and its text along with the scene. Whether that trade is worth taking is a judgement, so it is offered rather than assumed. Ayesha only for now: the full-frame path needs no engine knowledge and would probably work on the other two unchanged, but nothing has been measured there.

MSAA joins it as the other antialiasing option, and both are set from the launcher. Ayesha already multisamples its 3D scene at 4x, so the MSAA setting raises or lowers what the engine is doing rather than adding something new, and *Game default* and *Off* are different answers. Supersampling is implemented but currently disabled: its attachment point turned out to be wrong for this engine, and the failure is a black screen rather than a missing effect. See `WORK_DOC.md`.

The other Ayesha fixes are enabled by default. Escha & Logy and Shallie are hard-gated off for engine-specific fixes until their LTGL/KTGL paths have been investigated.

### Launcher

The custom launcher is implemented but not yet release-validated. It is intended to provide:

- Resolution, including an Auto entry that follows the desktop mode.
- Windowed or fullscreen mode.
- Language selection between the English and multilingual executables.
- Character-outline selection from the game's own settings.
- Ayesha's high-resolution and atlas-cache indicators where applicable.
- Access to Koei Tecmo's original launcher and settings editor.
- Play with the mod, Play without the mod, and SkipLauncher behavior.

Do not treat the launcher as validated until the session described in `WORK_DOC.md`, "The launcher window", has been completed.

## Background

The Dusk ports are not one engine target:

- Ayesha uses a PhyreEngine-derived, old-MSVC-CRT build. Its font-atlas and text-rendering path is the same code path used by the Arland DX games.
- Escha & Logy and Shallie use the newer LTGL/KTGL engine and UCRT builds. Their text layer has no verified homologue of Ayesha's menu path.
- The source is split by engine, but one `d3d11.dll` covers all three games. The capability matrix and executable fingerprint prevent a fix from crossing that boundary.

## Installation on Windows

1. Open the game's installation directory from Steam with **Manage -> Browse local files**.
2. Copy `d3d11.dll` beside the game executable. Add `dusk-fix-launcher.exe` and `msimg32.dll` when using the custom launcher.
3. Launch the game normally through Steam.

The Ayesha fixes are enabled automatically. Escha & Logy and Shallie remain unmodified by engine-specific fixes.

### Wine and Proton

Copy the files as above, then add this to the game's Steam launch options:

```text
WINEDLLOVERRIDES="d3d11=n,b" %command%
```

## Safety

The mod follows three rules:

- It never edits or redistributes Koei Tecmo executables. Hooks and launcher redirection exist only in the running process.
- Game-code hooks install only for recognized executable names, `.text` sizes, and verified prologues. Unknown builds are left alone apart from normal D3D11 forwarding.
- Removing `d3d11.dll` restores the game's normal behavior on the next launch. The launcher files are optional, and a partial launcher install leaves the stock front-end available.

The mod does not read or write save files, collect usage data, update itself, or connect to the internet in the background. It creates `dusk-fix.ini` beside the game when needed.

## Configuration

Normal settings live in `dusk-fix.ini`. The custom launcher is the intended way to edit the game's display and language settings. Manual configuration and launcher behavior are documented in [ADVANCED.md](ADVANCED.md).

## Build

Build instructions for Windows and Linux are in [BUILDING.md](BUILDING.md). The Linux script cross-compiles all three Windows targets with MinGW in the shared `atfix-build` container and checks the required proxy exports.

## Documentation

- [ADVANCED.md](ADVANCED.md) covers manual configuration, launcher behavior, and troubleshooting.
- [TECHNICAL.md](TECHNICAL.md) records finalized implementation details, measured behavior, executable fingerprints, and safety boundaries.
- [WORK_DOC.md](WORK_DOC.md) is the ongoing investigation record. It includes open work and unvalidated experiments and should not be read as a list of release guarantees.
- [CHANGELOG.md](CHANGELOG.md) records the planned first release and later changes.

## Credits

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) synchronization implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) supplied prior Map/Unmap coherence work and the old-Arland rendering correction that this project ports and refines. Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) is important prior work, but none of its code is used here; inspecting it did confirm two useful facts about SMAA on these games, that the same MIT reference shader is the right one and that the injection point is on the deferred context. [SMAA](https://github.com/iryoku/smaa) is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (MIT); its reference shader and the precomputed `AreaTex`/`SearchTex` lookup textures are vendored unchanged under `vendor/smaa`, and this project adds only the runtime integration. MinHook is by Tsuda Kageyu and contributors.

The Dusk-specific reverse engineering, measurements, and integration were carried out by Nico Verbruggen with assistance from large language models. See [TECHNICAL.md](TECHNICAL.md) for provenance and implementation details.

## License

See [LICENSE](LICENSE) for the MIT and zlib license terms applying to the respective source files.
