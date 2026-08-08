# Atelier Dusk Fixes

Performance and rendering fixes for the Steam releases of **Atelier Ayesha DX, Atelier Escha & Logy DX, and Atelier Shallie DX**, the Dusk trilogy.

For the Arland trilogy games, please see [this repository instead](https://github.com/nicoverbruggen/atelier-arland-fixes).

### Fixes

These are enabled by default, as they are crucial fixes.

| Feature | Ayesha | Escha & Logy | Shallie |
|---|:---:|:---:|:---:|
| New launcher | ✓ | ✓ | ✓ |
| Fixed menu performance | ✓ | N.A. | N.A. |
| Fixed high-resolution 3D rendering | ✓ | N.A. | N.A. |
| Fixed frame-rate dependent logic | ✓ | ✓ | ✓ |
| Fixed some typos | N.A. | ✓ | ✓ |
| Critical data loss fix | N.A. | ✓ | ✓ |
| Local crash logging | ✓ | ✓ | ✓ |
| Various small bug fixes | ✓ | ✓ | ✓ |

If the game crashes, the mod appends a report to `dusk-fix.log` that helps pinpoint the cause. Include that file and your settings when reporting the problem, since these logs are never sent anywhere.

### Enhancements

These things were not part of the original games, but were added with the mod. These can be turned off.

| Feature | Ayesha | Escha & Logy | Shallie |
|---|:---:|:---:|:---:|
| Edge smoothing (SMAA) | ✓ | ✓ | ✓ |
| Sharpening | ✓ | ✓ | ✓ |
| Supersampling | ✓ | ✓ | ✓ |
| Skip the startup logos | ✓ | ✓ | ✓ |
| Skip the opening movie | ✓ | ✓ | ✓ |

## Configuration

The launcher is the intended way to change anything. It writes the game's own `Setting.ini` and the mod's `dusk-fix.ini`, and it only shows options the game it sits next to actually supports.

Two notes worth having:

- **Supersampling costs the most and does the most.** These games alias badly in ways edge smoothing cannot reach, because what aliases is detail inside textures and along alpha-tested edges rather than the outlines of models.
- **On Escha & Logy and Shallie it also improves the interface**, which is laid out in a fixed 1920x1080 space and magnified to fill the screen. That looks cleanest when the render resolution is a whole multiple of it: the 150% setting on a 1440p screen, or 200% on a 1080p one.

[ADVANCED.md](ADVANCED.md) documents the configuration files, the one-session diagnostic switches and troubleshooting. [TECHNICAL.md](TECHNICAL.md) describes how each fix works and why it takes the form it does.

## Credits

> [!NOTE]
> I did the reverse engineering and integration behind this mod, using large language models from OpenAI and Anthropic throughout to analyze the games, develop the fixes, and bundle the improvements together. I believe that LLMs were used responsibly in this project.

This mod is a spin-off of [atelier-arland-fixes](https://github.com/nicoverbruggen/atelier-arland-fixes), which is where its architecture comes from. It was inspired by, and consulted, prior work by:

- Philip Rebohle's [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix)
- TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix)
- Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/)

The bundled SMAA anti-aliasing is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez ([SMAA](https://github.com/iryoku/smaa), MIT), vendored unchanged. The sharpening pass implements AMD's [FidelityFX Contrast Adaptive Sharpening](https://gpuopen.com/fidelityfx-cas/). [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

See [TECHNICAL.md](TECHNICAL.md) for the full implementation details and the evidence behind them.

## License

See [LICENSE](LICENSE) for the MIT and zlib license terms applying to the respective source files. 

Since Ayesha's game engine is quite similar, see [arland-atelier-fix](https://github.com/nicoverbruggen/atelier-arland-fixes) for a version compatible with the Arland games, as this mod is a spin-off of the Arland trilogy one.
