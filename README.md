# Atelier Dusk Fixes

Performance and rendering fixes for the Steam releases of **Atelier Ayesha DX, Atelier Escha & Logy DX, and Atelier Shallie DX**, the Dusk trilogy.

For the Arland trilogy games, please see [this repository instead](https://github.com/nicoverbruggen/atelier-arland-fixes).

## What is included

### Launcher

The **included launcher** replaces the standard window for the Dusk games and gives you more control over your game experience. It also lets you tweak various **graphics settings** that are added by this mod.

### List of fixes and improvements

Corrective fixes are enabled by default wherever they apply. Options that deliberately suppress the games' startup presentation are marked optional and remain off until selected.

| Fix                                        | Ayesha | Escha & Logy | Shallie |
|--------------------------------------------|:------:|:------------:|:-------:|
| Much faster menus (removed stutter)        |   ✓    |      —       |    —    |
| Higher resolution rendering                |   ✓    |      —       |    —    |
| Correct behaviour at high refresh rates    |   ✓    |      ✓       |    ✓    |
| Fixed stutter with no controller connected |   ✓    |      ✓       |    ✓    |
| Fixed some typos                           |   —    |      ✓       |    ✓    |
| Fixed "system data" corruption bug         |   —    |      ✓       |    ✓    |
| Correct picture shape on non-16:9 displays |   —    |      ✓       |    ✓    |
| Optional startup logo and intro-movie skip |   ✓    |      ✓       |    ✓    |
| Various game-specific bug fixes            |   ✓    |      ✓       |    ✓    |
| Local crash logging                        |   ✓    |      ✓       |    ✓    |

If the game crashes, the mod appends a report to `dusk-fix.log` that helps pinpoint the cause. Include that file and your settings when reporting the problem, since these logs are never sent anywhere.

### List of graphics enhancements

These things were not part of the original games, but were added with the mod. Each one can be turned on or off in the launcher. Edge smoothing is on to begin with; supersampling and sharpening are optional.

| Enhancement                                | Ayesha | Escha & Logy | Shallie |
|--------------------------------------------|:------:|:------------:|:-------:|
| Edge smoothing (SMAA)                      |   ✓    |      ✓       |    ✓    |
| Supersampling (internal render resolution) |   ✓    |      ✓       |    ✓    |
| Sharpening                                 |   ✓    |      ✓       |    ✓    |
| Higher-resolution shadows                  |   ✓    |      —       |    —    |

## Installation on Windows

> [!IMPORTANT]
> This mod is a replacement for `atelier-sync-fix` and Atelier Graphics Tweak (`AGT`), so remove those mods first if you have them installed.

1. Open the game's installation directory from Steam by selecting **Manage → Browse local files**.
2. Copy the contents of the latest release (`d3d11.dll`, `dusk-fix-launcher.exe`, `msimg32.dll` and `dusk-fix.ini`) into that directory, beside the game's own executables. If you are updating an existing install, keep the `dusk-fix.ini` you already have, since the bundled one is only the defaults.
3. Launch the game normally through Steam. The mod's launcher should open now instead of the original one.

## Installation on Linux (Proton)

The mod works correctly under Proton, and the games keep using Steam as usual. There is one extra step: Wine ships its own `d3d11` and `msimg32`, and it prefers them over the files in the game folder, so you have to tell it not to.

In **Properties → General → Launch Options**, add:

```text
WINEDLLOVERRIDES="d3d11,msimg32=n,b" %command%
```

Keep `%command%` at the end. After this, the game should use the new launcher that the mod provides.

## Safety

I have done my best to make the mod as safe as possible. Here's what you should know:

### Policy #1: Keep the original game files untouched

Like other DLL-based game fixes, this mod is loaded by the game and changes how parts of it work while it is running. It does not permanently patch the games: the changes disappear when you close the game, and the original executables and game assets are never edited.

### Policy #2: Safety checks and easy removal

In practical terms:

- Address-based and Direct3D fixes require an exact executable name and build fingerprint. If either does not match, those fixes are skipped and Direct3D is only forwarded. Two startup-window corrections must be hooked before the normal fingerprint gate runs; they change a call only when the game module, window class/brush or measured window size matches their narrow runtime checks, and otherwise pass it through unchanged.
- It does not read or write your save files, collect usage data, update itself or connect to the internet in the background.
- The only files it normally creates or updates are its settings and diagnostic logs. Changing options in the settings launcher also updates the game's own settings file.
- **Play without the mod** starts the game with the fixes disabled. Removing or renaming the mod's DLL files returns the game to normal the next time it starts.

Like any DLL mod, this is executable code that runs with the same access as the game. Download it only from this repository's [official releases](https://github.com/nicoverbruggen/atelier-dusk-fixes/releases), or build it from source.

### Policy #3: Public source and build process

The complete source code and the steps GitHub uses to build each release are public. You can also make your own build: the repository builds with Meson and a MinGW cross toolchain, and `scripts/build_linux.sh` runs the whole thing.

> [!TIP]
> If you do not read code yourself, an LLM (like ChatGPT or Claude) can help review the repository for obvious red flags and explain whether it appears to do what this page claims. That can be a useful second opinion, but it is not a guarantee and cannot prove that a downloaded file was built from the published source. You can ask it to inspect the built files, however this may use up a lot of tokens.

## Configuration

Use the launcher. It writes the game's own `Setting.ini` and the mod's `dusk-fix.ini`, and only shows options the game it sits next to supports. `dusk-fix.ini` ships with every default filled in, so it also serves as the list of what can be set.

## Credits

> [!NOTE]
> I did the reverse engineering and integration behind this mod, using large language models from OpenAI and Anthropic throughout to analyze the games, develop the fixes, and bundle the improvements together. I believe that LLMs were used responsibly in this project.

This mod is a spin-off of [atelier-arland-fixes](https://github.com/nicoverbruggen/atelier-arland-fixes), which is where its architecture comes from. It was inspired by, and consulted, prior work by:

- Philip Rebohle's [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix)
- TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix)
- Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/)

The bundled SMAA anti-aliasing is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez ([SMAA](https://github.com/iryoku/smaa), MIT), vendored unchanged. The sharpening pass implements AMD's [FidelityFX Contrast Adaptive Sharpening](https://gpuopen.com/fidelityfx-cas/). [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

## License

See [LICENSE](LICENSE) for the MIT and zlib license terms applying to the respective source files.

Since Ayesha's game engine is quite similar, see [atelier-arland-fixes](https://github.com/nicoverbruggen/atelier-arland-fixes) for a version compatible with the Arland games, as this mod is a spin-off of the Arland trilogy one.
