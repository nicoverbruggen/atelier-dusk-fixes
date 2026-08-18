# AGENTS.md

## Project scope

Fixes for the Steam Dusk-trilogy DX ports: Atelier Ayesha DX, Atelier Escha & Logy DX, Atelier Shallie DX. This project is still pre-release. The detailed investigation record and the work queue are kept privately and are deliberately not published here.

## Source layout

Split by engine, because the trilogy spans two and they share nothing a fix can reach:

- `src/core/` — engine-agnostic: the D3D11 proxy (`main.cpp`), engine dispatch (`engine.cpp`), the capability matrix (`game.cpp`), the `dusk-fix.ini` layer (`config.cpp`), D3D11 vtable ownership and hook installation (`d3d11_hooks.cpp`), the high-resolution fix and its render-target census (`highres.cpp`), the verdict on which surface is the scene (`scene_pass.cpp`), the per-engine answers the pre-UI pass needs (`scene_policy.h`), the antialiasing features (`smaa.cpp`, `supersample.cpp`, `sharpen.cpp`), logging.

  **`d3d11_hooks.cpp` is the only place that hooks a D3D11 vtable.** Features own their detours and their policy and declare them in a "wiring for d3d11_hooks.cpp" section of their own header; they do not call MinHook. Two modules hooking one vtable is how an enable/disable race gets written by accident, and how a half-installed set survives a failure that should have rolled everything back. Add a slot by adding a row to a spec table there, not by hooking from a feature.

  Rendering features may still need one engine-specific decision each. Keep it out of core: `src/core/scene_pass.h` takes a `SceneTargetTest` callback for "which bind is the scene", and each engine supplies its own — `src/engines/phyre/scene_target.cpp` for Ayesha, `src/engines/ktgl/scene_target.cpp` for Escha & Logy and Shallie. Core declines and says so in the log when no engine has registered one, which is now a fallback rather than the state any shipped game is in. Both SMAA's pre-UI pass and supersampling depend on that one answer.
- `src/engines/phyre/` — Ayesha (PhyreEngine, old MSVC CRT). Arland ports live here: the atlas cache, field physics, and the scene-target rule.
- `src/engines/ktgl/` — Escha & Logy and Shallie (KTGL, UCRT). Fingerprinting and the engine-specific address fixes, including loading text, startup flow, save integrity and world-map movement. Each detour or patch is gated by the recognized executable row and its own expected-byte window.

  Plural on purpose. `src/core/engine.{h,cpp}` is the **dispatch layer** — it resolves which engine this process is and forwards to one module. `src/engines/` holds the modules it forwards to. Singular for the dispatcher, plural for the implementations.
- `src/launcher/` — both launcher pieces, neither of which is an engine module and neither of which shares code with the game DLL: `launcher_gui.cpp` is the 64-bit `dusk-fix-launcher.exe` settings window, and `launcher_proxy.cpp` is the 32-bit `msimg32.dll` the games' own front-ends load. They agree on ini key names and nothing else.

One `d3d11.dll` covers all three games. Do not split it per game or per engine: address-based and Direct3D fixes are gated on both the capability matrix and an exact executable fingerprint. The only exception is a small set of window-API hooks that must be installed before D3D11 initialization; those may change a call only when narrow runtime facts identify the game window, and must forward everything else untouched. `msimg32.dll` is a separate target only because the front-ends that load it are 32-bit processes.

Neither engine module may include the other's headers, and no address pack belongs in `src/core`. **Nor may `src/core` name an engine module.** Only `engine.cpp` may, and only to dispatch: it resolves the running executable and returns that engine's `SsaaPolicy` and `ScenePolicy`. Core asking one engine a question about the other is how Ayesha's pre-UI pass came to be gated on a KTGL module reporting itself idle, and how a decline meant for Ayesha was logged in KTGL's words.

User-facing options go in `dusk-fix.ini` through the capability matrix's `Descriptor`; environment switches are diagnostics and must not be given an ini key. `[Diagnostics] VerboseLogging` is the one diagnostic with a key, because it only decides how much the mod writes about itself and costs a user nothing to turn on when a bug report asks for it. A diagnostic that makes the game slower keeps its environment switch.

**No ini ships. The mod writes its own.** `configPath()` creates `dusk-fix.ini` when it is absent and seeds `[Launcher] SkipLauncher`; every other key is seeded lazily by the read that wants it, with the capability matrix's default for the game actually running. The settings launcher then writes the file whenever the user saves. A shipped file could not do better: it would carry one set of values for three games, including keys a given game ignores.

That makes the launcher the option surface. A key the launcher does not offer is reachable only by someone who already knows its name, so adding an option means adding its control, and the default a user sees before the DLL has ever run is the launcher's own fallback rather than anything in the repository.

An option may still be deliberately ini-only. What that costs is discoverability, so it is a decision rather than an omission, and `scripts/check_option_surface.py` holds the list with the reason for each. Drifting into it is the thing to avoid: a key read behind a condition never appears for the user who leaves that feature off, and nobody has to decide anything for it to happen.

This is the sibling of `../atelier-arland-fixes`. Read that repository's `AGENTS.md` first: its architecture (d3d11 proxy, capability matrix, hook idioms) is the template for this project.

## Documentation policy

This repository publishes **no investigation record**. The evidence layer — disassembly, addresses, struct layouts, derivations, open questions and unvalidated ports — lives privately alongside the work queue, outside this repository. Do not recreate a `WORK_DOC.md` here, and do not add a second TODO or handoff file.

**Never name that private location anywhere in this repository** — not in source comments, not in documentation, not in this file. A reader of the published repository should never be pointed at a document they cannot open, nor learn that one exists by name or path.

**The code is the technical record.** There is no prose document describing how the fixes work, and adding one is not the default answer to "this needs explaining". A fix's header explains what the defect is, what the correction does, and why it takes that shape; the evidence that settled it belongs there too, next to the thing it justifies. A reader who opens `src/core/sharpen.h` should not need anything else to understand the pass.

That places a real obligation on comments. They carry what a separate document would have carried, so they are written for someone who can program but does not know this engine, and they record the reasoning rather than restating the code. A measurement that decided a design goes in the header that design lives in.

### How much a comment says

The everyday style is already right: half of all comment blocks are three lines and nine in ten are ten lines or under. What follows is about the few that carry an essay.

**One explanation, one place.** A mechanism is explained in the header of the file that implements it. Everywhere else names it and stops. When a passage explains a mechanism and then says "See `supersample.h`", the text above the pointer is a second copy: keep the pointer and delete the copy. Before deleting, check the fact really does exist at the destination, and move it there when it does not.

**A comment answers what a reader asks where it sits.** At the capability matrix a reader asks what the table is, how to read a cell, and what to do when adding a row. They do not ask how supersampling identifies the composite. Explaining another file's mechanism is the sign that the text is in the wrong file.

**A file header is different, and may be long.** The code is the technical record, so the header of the file that owns a fix carries what a separate document would: the defect, the correction, why it takes that shape, and the measurement that settled it. `src/core/sharpen.h` is the model.

**Do not trim by the ruler.** Length finds candidates. It does not judge them. A long block that is entirely about the thing its file implements is doing its job.

The only user-facing document is `README.md`, which lists what the mod does per game. What can be set is the settings launcher, not a file.

## Game copies

Never modify or redistribute Koei Tecmo executables; launcher/game mutations stay in memory and signature-gated, following the Arland rules. Deploying means copying `build64/d3d11.dll` next to the game executable, plus `build64/dusk-fix-launcher.exe` and `build32/msimg32.dll` when the launcher is in scope.
