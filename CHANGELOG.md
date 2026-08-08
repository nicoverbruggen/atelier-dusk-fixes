# Changelog

## v0.1 (unreleased)

The first release. This is what the mod does, rather than a list of changes: there is no earlier version to compare it against.

### Corrections

- **Menu performance in Ayesha.** The game builds its menus by mapping the same three font atlases over and over. Those reads are served from frame-scoped snapshots instead. A representative menu that took 248.4 ms builds in 38.2 ms.
- **High-resolution rendering in Ayesha.** Several internal scene targets stay at 1920x1080 whatever resolution is selected, so a 4K picture is a 1080p picture enlarged. They now follow the chosen resolution, along with the matching blur target, viewport and scissor. Nothing changes at or below 1080p.
- **Frame-rate dependent movement in Ayesha.** The field controller discards small movements using a distance that was only correct at the original frame cadence, which makes the character jitter vertically while standing still at high refresh rates. The threshold is rescaled with frame time and a genuinely resting character is held still.
- **The travel-map cursor in Ayesha.** It moved a fixed distance per frame rather than per second, crossing the map more than three times too fast at 200 Hz. At 60 fps and below nothing changes.
- **Synthesis animation speed in Escha & Logy and Shallie.** The product-card animation advances once per drawn frame, so above 60 fps it runs fast: about 3.3 times too fast at 200 Hz. It now advances at the rate it was authored for.
- **Save data protection in Escha & Logy and Shallie.** A failed load of the system save reports success, installs blank data over the live settings, and the next settings change writes that back over the file. The mod refuses to save system data until a load genuinely succeeds.
- **"Loadning system data." in Escha & Logy and Shallie.** The English status line on the games' first screen misspells "Loading".
- **A black startup screen in Ayesha.** The window is filled with mid-grey for about a second before the first frame arrives. Black is what the game fades up from.

### Graphics options

- **Supersampling**, in all three games. The scene is rendered above the display resolution and resampled down, which is the only antialiasing that improves texture interiors and alpha-tested edges as well as model silhouettes. Multipliers are 1.25x, 1.5x, 2x, 3x and 4x. In Escha & Logy and Shallie the interface is included, so its artwork is sharper at high resolutions too.
- **Edge smoothing (SMAA)**, in all three games. Cheap, and it reaches edges multisampling cannot. In Ayesha it runs before the interface is drawn, so menu text stays sharp. In Escha & Logy and Shallie it runs over the finished picture and softens text along with everything else, which is why it is off there unless asked for.
- **Anisotropic filtering** at 16x in Ayesha. It costs nothing measurable, so it has no setting.

Multisampling is deliberately not offered. It cannot reach what actually aliases in these games, which is detail inside textures and along alpha-tested edges, and the engines never render into the multisampled targets they allocate.

### Convenience

- **A settings launcher**, in place of Koei Tecmo's. Resolution, window mode, language, character outlines and every option above in one window, with the game's own settings editor and launcher still reachable from it. It can be skipped so that Play in Steam goes straight into the game.
- **Skipping the startup logos and the opening movie** in Ayesha. Neither makes the game start sooner: the logos play while it loads.
- **A steady control-hint panel** in Shallie, which otherwise replays its slide-in entrance every time the interface is rebuilt.

### Scope

- Escha & Logy and Shallie run a different engine from Ayesha, and several corrections above belong to one or the other. The mod recognises which game it is running in and applies only what belongs there; an unrecognised executable is left alone apart from normal Direct3D forwarding.
- Options a game does not support are not shown in the launcher.
- The first public release is not cut by this repository state. The remaining release bookkeeping has to be completed first.

Implementation detail for each of these is in [TECHNICAL.md](TECHNICAL.md). The configuration files, the one-session diagnostic switches and the troubleshooting notes are in [ADVANCED.md](ADVANCED.md).
