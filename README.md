# Atelier Dusk Fixes

Performance and rendering fixes for the Steam releases of **Atelier Ayesha DX, Atelier Escha & Logy DX, and Atelier Shallie DX**, the Dusk trilogy.

### Fixes

These are enabled by default, as they are crucial fixes.

| Feature | Ayesha | Escha & Logy | Shallie |
|---|:---:|:---:|:---:|
| New launcher | ✓ | ✓ | ✓ |
| Fixed menu performance | ✓ | N.A. | N.A. |
| Fixed high-resolution 3D rendering | ✓ | N.A. | N.A. |
| Fixed frame-rate dependent movement | ✓ | — | — |
| Fixed frame-rate dependent animations | — | ✓ | ✓ |
| Fixed some typos | N.A. | ✓ | ✓ |
| Black instead of grey startup screen | ✓ | — | — |
| Critical data loss fix | N.A. | ✓ | ✓ |
| Local crash logging | ✓ | ✓ | ✓ |

If the game crashes, the mod appends a report to `dusk-fix.log` that helps pinpoint the cause. Include that file and your settings when reporting the problem, since these logs are never sent anywhere.

### Enhancements

These things were not part of the original games, but were added with the mod. These can be turned off.

| Feature | Ayesha | Escha & Logy | Shallie |
|---|:---:|:---:|:---:|
| Edge smoothing (SMAA) | ✓ | ✓ | ✓ |
| Supersampling | ✓ | ✓ | ✓ |
| Skip the startup logos | ✓ | ✓ | ✓ |
| Skip the opening movie | ✓ | ✓ | ✓ |
| Steady control hints | N.A. | N.A. | ✓ |

## Configuration

The launcher is the intended way to change anything. It writes the game's own `Setting.ini` and the mod's `dusk-fix.ini`, and it only shows options the game it sits next to actually supports.

Two notes worth having:

- **Supersampling costs the most and does the most.** These games alias badly in ways edge smoothing cannot reach, because what aliases is detail inside textures and along alpha-tested edges rather than the outlines of models.
- **On Escha & Logy and Shallie it also improves the interface**, which is laid out in a fixed 1920x1080 space and magnified to fill the screen. That looks cleanest when the render resolution is a whole multiple of it: the 150% setting on a 1440p screen, or 200% on a 1080p one.

[ADVANCED.md](ADVANCED.md) documents the configuration files, the one-session diagnostic switches and troubleshooting. [TECHNICAL.md](TECHNICAL.md) describes how each fix works and why it takes the form it does.

## License

See [LICENSE](LICENSE) for the MIT and zlib license terms applying to the respective source files.
