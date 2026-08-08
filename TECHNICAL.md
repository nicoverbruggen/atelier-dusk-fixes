# Technical overview

This document records finalized, measured behavior for the first planned Dusk release. Features that are still opt-in pending measurement are named as such; anything not described here is not a release guarantee.

## Scope and engines

One 64-bit `d3d11.dll` recognizes all six Dusk game executables. Ayesha is the PhyreEngine-derived target and uses the atlas, field, and scene modules under `src/engines/phyre`. Escha & Logy and Shallie use LTGL/KTGL and are served by `src/engines/ktgl`.

The capability matrix in `src/core/game.cpp` is the source of truth. Every engine-specific feature is hard-off for an unsupported title, and an unrecognized build receives only normal D3D11 forwarding. Ayesha has the atlas cache, high-resolution correction, field correction, antialiasing, and travel-map correction enabled by default. Escha & Logy and Shallie have the loading-text correction, the system-save guard, and the synthesis-animation correction enabled by default, and can additionally be given SMAA, supersampling and sharpening. Target census is available as a diagnostic for all three games.

## Font-atlas cache

Ayesha builds menus by repeatedly mapping the same three 512x512 font atlases. A representative unmodified menu drain made 2,385 candidate locks and took 248.4 ms. The cache reduced that drain to 38.2 ms, an 85% reduction. In the measured session it served 63,517 cache hits against 3,029 real reads, a 95.5% hit rate.

The cache is frame-scoped because 72% of candidate locks occurred outside the resource queue drain. It is eligible only from the verified text-renderer call path and for 512x512 atlas textures. Snapshots are created only from read locks, while existing snapshots can serve both read and write modes. An unmatched real unlock invalidates that texture's snapshot, and a partial hook installation remains pass-through rather than enabling half the cache.

The English and multilingual Ayesha executables use separate address packs. The four required anchors are the queue drain, text renderer, atlas lock, and atlas unlock stub. The cache is hard-off for Escha & Logy and Shallie because their text-rendering layer has no equivalent.

`DUSK_ATLAS_STATS`, `DUSK_ATLAS_TRACE`, `DUSK_ATLAS_VERIFY`, `DUSK_ATLAS_CENSUS`, and `DUSK_D3D11_WRITE_PROBE` are diagnostic paths only. Their behavior and interpretation are documented in [ADVANCED.md](ADVANCED.md).

## High-resolution rendering

Ayesha accepts a high display resolution but leaves several internal scene targets at 1920x1080. The correction uses a narrow `CreateTexture2D` classification:

- The first suitable depth target becomes the main render size only when its shape matches the swap chain or is a valid 16:9 target at least 1920x1080.
- Empty 1920x1080 render and depth targets are resized to the main render size.
- The exact 960x540 typeless BGRA blur target is resized to half-size.
- Existing data, unrelated textures, smaller pyramid levels, and the 1024x1024 shadow map pass through.
- If an enlarged target is rejected, creation is retried with the game's original descriptor.

Viewport and scissor correction is deferred until draw time, when the bound target can be checked. Immediate and deferred context vtables are hooked separately because Ayesha submits its draws through a deferred context. The correction was confirmed at 3840x2160: the main render size followed the swap chain, and the in-game frame was sharper and correctly framed. At 1080p, the hooks remain passive because no target is larger than the pinned size.

Escha & Logy and Shallie do not have this defect. Both size the swap chain and every internal render target from the same two `Setting.ini` values, so no correction is applied and none is needed.

`DUSK_HIGHRES=0` disables the correction for one session. It has no ini key because selecting the output resolution is already the user decision the correction implements.

**The travel-map cursor** advances a fixed distance per frame in Ayesha and in Escha & Logy, so it crosses the map faster the higher the refresh rate — more than three times too fast at 200 Hz. The mod captures the frame delta the update already receives and rescales the step the mover applied by `min(dt * 60, 1)`, writing the corrected value to both the state's own copy and the render node it publishes to. At 60 fps and below the factor is 1 and the shipped behaviour is preserved exactly.

Shallie is not corrected, and that is the evidence rather than an omission: the same function there takes the frame delta as a parameter and multiplies by it, while Escha's takes no float parameter at all. Reading both executables is what established the defect. `DUSK_WORLDMAP=0` stands the correction down; `DUSK_WORLDMAP_PROBE=1` logs measured units per second, which should scale with refresh rate before the correction and not after it.

**The startup screen** is a flat fill for about a second before the first frame arrives, and the mod replaces the brush at the moment the game registers its window class. Two rows cover the trilogy: Ayesha registers `KTGL.A11` with the grey stock brush, and Escha & Logy and Shallie register `ElixirFramework` with the white one. A class is rewritten only when both its name and its brush match a row, so the correction stands down by itself if a build ever stops doing it. `DUSK_WINDOW_CLASS_TRACE=1` names what a build actually registers.

## Antialiasing

**Supersampling** is available in all three games and confirmed in game in each. It renders the scene above the display size and resamples it down with a box filter sized to the ratio, with sharpening folded into the same pass. Because the resample is the mod's own, fractional multipliers are usable; the ladder is 125%, 150%, 200%, 300%, and 400%. It is opt-in everywhere: 200% is four times the shaded pixels.

The two engines reach it by opposite routes, and only the plumbing differs. On Ayesha the engine pins its scene targets, so the mod enlarges them itself and the game's own `Setting.ini` keeps the resolution the player chose. On Escha & Logy and Shallie the engine already sizes every full-frame target from its own `Setting.ini`, so the launcher writes that file at base times factor and records the base separately; the mod then clamps the swap chain back to the base, resolves the oversized frame into it at the composite's own sample, and corrects the composite's viewport, which the engine still sets from the size it believes it is rendering at.

In both cases the composite is identified positively, by the bind whose color target is the swap-chain back buffer, rather than inferred from when the engine stops drawing into the scene.

On the KTGL games the whole frame is supersampled rather than the scene alone, because the interface is composed in the same coordinate space. That costs more, and it is also the only setting that improves the interface at high resolutions. Those games lay the interface out in a fixed 1920x1080 space and magnify its artwork to fill the frame, so the picture is cleanest when the render resolution is a whole multiple of 1920x1080: at 2560x1440 that is the 150% setting, and at 1920x1080 it is 200%.

**SMAA** is available in all three games, and where it runs differs.

It runs pre-UI in all three, and each engine answers "where is that moment" for itself. The answers are a table of function pointers per engine (`src/core/scene_policy.h`), resolved from the running executable by `engine.cpp`, which is the only file allowed to name an engine module.

On Ayesha the moment is the bind that leaves the surface the frame's 3D pass was drawn into. That surface is identified by counting draws, not by its shape: the engine ping-pongs between two identically-shaped scene colour targets through its post-processing chain, and a size-and-format rule matches both of them — correctly, since both are scene targets, but it cannot say which one the player is about to be shown. The transition itself fires 5 to 22 times a frame for the same reason. A run of at least 24 draws separates the 3D pass from the handful of full-screen draws each post-processing step issues; a measured frame put the 3D pass at 154. This is the same rule the Arland project uses in all three of its games.

On Escha & Logy and Shallie the moment is the first draw into the screen-sized typeless colour surface that is bound after the frame's main geometry run. A copy of that surface taken after that one draw is the finished scene with no interface in it; the fourteen draws that follow are the interface. The rule is structural rather than an address — a run of hundreds of draws, then the next `RENDER_TARGET|SHADER_RESOURCE` typeless bind at render size — so it carries across both games and both builds.

**With supersampling engaged, neither engine anchor runs.** The downscale performs both passes itself, on the display-sized result and still before the interface, which is where a morphological filter belongs: SMAA's search distances are counted in pixels, so on a 2x scene target every edge is twice as wide as its thresholds expect. Two pre-UI passes would not stack, they would alternate — both claim the same once-per-frame latch, so whichever reached it first would win that frame and the passes would flip between the two resolutions from one frame to the next.

The bloom composite in `PostEffectGlow.kps` is **not** that moment, and it is the obvious wrong answer: it is easy to identify by shader checksum, it binds once per frame in gameplay, and its own render target already contains the interface. Antialiasing there does what the Present-time pass does.

Where no pre-UI moment is claimed, SMAA falls back to Present over the finished frame and the interface is antialiased with the scene. The log distinguishes them: `KTGL pre-UI: active` and `SMAA: pre-UI active` for the first, `SMAA: running at Present over the finished frame` for the second.

**Sharpening** is contrast-adaptive (AMD's CAS) and runs on the same surface immediately after the antialiasing, wherever that is — the engine anchor, or the downscale when supersampling is engaged — so it is always before the interface. Under supersampling it is a second sharpen on the frame, because the resample folds one in already; that is deliberate, since the two are separate settings and the slider is meant to add to what supersampling does rather than be absorbed by it. It is independent of edge smoothing in every direction except order: it works with that pass off, it never runs before it, and turning edge smoothing off does not take it with it. The strength is one of four presets — off, low, medium, high — written to the ini as the percentage the DLL reads. The launcher matches that key by number and rounds to the nearest preset, so a hand-edited value or one set through `DUSK_SHARPEN` is not lost.

The percentage is not AMD's sharpness parameter. It maps onto the kernel's peak weight over a range of 0.02 to 0.15, where AMD's own range is 0.125 to 0.2. The ceiling stops short of theirs because the slider is not the only sharpening in the frame whenever supersampling is on.

Both passes bind their render targets through the unhooked original rather than the vtable. Going through the mod's own detour tells supersampling which target is bound, so a scratch bound mid-downscale reads as the composite no longer being bound, and the composite's viewport correction then does not fire — the engine draws its oversized scene 1:1 into the clamped back buffer and the picture is cropped.

CAS rather than an unsharp mask because it takes its weight from the local minimum and maximum: flat regions are left alone and already-hard edges are sharpened *less* than mid-contrast ones, which is what makes it safe to leave on over 2013-era art being magnified.

Multisampling is deliberately not offered, and was removed rather than left switched off. It cannot reach what actually aliases in these games, which is detail inside textures and along alpha-tested edges; only supersampling resolves that. The engine's own multisampled targets are never rendered into, so there is no engine setting to turn up either.

**Anisotropic filtering** is upgraded to 16x on Ayesha, where it ships on and has no launcher control: it costs nothing measurable, and a setting with no trade in it is not a setting. On Escha & Logy and Shallie it is available but off by default, because those games' sampler use has not been censused and a blanket upgrade there could smear a lookup texture under magnification.

`DUSK_SSAA`, `DUSK_SMAA`, and `DUSK_ANISO` control each feature for one session.

## High-refresh field movement

Ayesha's field controller discarded sub-threshold movement using a distance that was correct only at the shipped frame cadence. A 144 Hz baseline measured 12-18 pixels of vertical character excursion while the character was horizontally at rest on the atelier steps. The motion had the sustained sawtooth shape expected from gravity accumulating against the collision threshold.

The correction has two coupled parts. The engine threshold is rescaled with frame time, and a stabilizer holds a genuinely resting grounded controller while pinning the air timer. The stabilizer refuses to install without the rescale. Both Ayesha executable builds have independently checked controller and collision anchors and the correction was confirmed in game with both halves active.

`DUSK_FIELD_ENGINE_FIX=0` disables the complete correction for one session. `DUSK_FIELD_STABILIZER=0` disables only the second half for comparison.

## Travel-map cursor

Ayesha's travel-map cursor moved a fixed distance per rendered frame rather than per elapsed second, so it crossed the map roughly three times faster at 200 Hz than the game was built for. The function that produces the step receives no frame time; its immediate caller has it and does not pass it on.

The correction captures the caller's frame time and rescales the step the game produced by `min(dt * 60, 1)`. At 60 fps and below that factor is 1 and shipped behavior is preserved exactly. Nothing is predicted or simulated: the game's own movement code runs untouched and only its output is scaled, so the corrected position cannot disagree with anything else that reads it. Both Ayesha builds are covered and the correction was confirmed in game.

`DUSK_WORLDMAP=0` disables it for one session.

## Synthesis animation cadence

Escha & Logy and Shallie advance the synthesis product-card animation from a fixed-timestep pump whose loop is entered unconditionally, so it steps at least once per rendered frame. At or below the authored 59.94 Hz cadence that is correct. Above it the pump's own count is always zero and it steps anyway, so the animation advances at the frame rate: roughly 2.4 times too fast at 144 Hz and 3.3 times too fast at 200 Hz.

The correction supplies the missing check and nothing else. It evaluates the game's own condition for whether a step is due; when one is, the original runs completely untouched and behaves as shipped, and when one is not, the elapsed time is banked for the next frame. Card positions are still published to the interface every rendered frame, so motion stays smooth at any refresh rate. Confirmed in game in both titles.

`DUSK_SYNTH_RATE=0` disables it for one session.

## System-save protection

Escha & Logy and Shallie can silently lose their system save data: settings, gallery, costumes, and bonus flags. The cause is not the writer. A failed load reports success, an empty buffer is installed over the live data, the loaded data is accepted without complaint, and the next settings change writes the resulting defaults back over a healthy file. No error is shown at any point.

The guard adds two checks, both of which only make the game more conservative about its own data. When a system-data load reports completion having read nothing, it is placed into the failure state the game already defines, so the caller skips installing the empty buffer and the live data is left alone. While that has happened, system-data saves are refused until a load succeeds, because that write is what would replace good data with defaults.

Ordinary game saves are deliberately untouched: both operations are shared between the two save kinds, and every path checks which kind it is handling first. All four executable builds are covered.

The defect was reproduced by truncating a system save to zero bytes, and the guard was confirmed to leave the file intact through a settings change that would otherwise have overwritten it.

`DUSK_SYSTEM_SAVE=0` disables the guard for one session.

## Loading-text correction

Escha & Logy and Shallie misspell "Loading" as "Loadning" in the status line on the first screen of the game. The word is a plain string literal in each executable, verified byte for byte before a 22-byte in-memory correction at startup. It is not present in any game data file, which is what makes it correctable without intercepting a file load. All four executable builds are covered, and the correction was confirmed on screen in both titles.

`DUSK_LOADING_TEXT=0` disables it for one session.

## Control-hint panel

Shallie's on-screen control hints slide into place from the edge of the screen, and replay that entrance whenever the containing interface is rebuilt. The correction hands the game's own easing code a larger frame time, so each hint lands on its target in a single frame using the game's own limits. Nothing is patched and the panel still appears and disappears exactly when it did.

This is a preference rather than a correction, so it ships off. `[Interface] SteadyControlPrompt` enables it; `[Interface] HideControlPrompt` selects the stronger behavior of not drawing the panel at all. Escha & Logy has no equivalent panel.

## Startup logos and the opening movie

Two independent skips, both opt-in and both on all three games. Neither shares an implementation between the two engines, and the logo skip does not even share a mechanism: what it writes on Ayesha is a phase, and on the other two an elapsed time.

**Ayesha's logos** belong to an object the application creates before it starts initializing the engine. Its update runs on the render thread and steps a phase sequence over fullscreen picture layers; the application does its whole engine and resource initialization while that animates, and only then waits for the sequence to report its terminal phase. Writing that phase releases the wait. The draw is suppressed as well, because forcing the phase stops the picture layers being ticked and a never-ticked layer's contents are not worth reasoning about.

Two consequences follow and neither is a defect. Skipping does not start the game sooner: the logos play while the game loads, so this shows a black screen for as long as loading genuinely takes. And the idle-title attract replay stops happening, because it waits on the same field.

**Escha & Logy's and Shallie's logos** have no such object; neither executable carries a logo class at all. What they have is a three-step sequence owned by the title screen, whose steps are a health and safety notice, the Koei Tecmo logo and the Gust logo, each holding for a fixed time taken from a table in the executable. All three states call one shared body to set the step up, and that body finishes by zeroing an elapsed-seconds field; each state's update then adds the frame time to it and asks a check whether the hold has run out.

The skip is one write. After the shared body has set the step up and zeroed the timer, the mod puts a large number back. The next update finds the time exhausted and advances, exactly as it would have on its own. The image still loads and the layer stays real, so nothing downstream sees a state it was not written for. The value is large rather than exact because the check compares against more than one field of that table row, and a value past all of them ends the step however the comparison is read.

Here the skip does start the game sooner. Removing the holds is not enough on its own, because each step also fades in and fades out and the update will not advance while either runs, so the skip additionally answers "is a fade running" with no for the one image object the logo step owns. Every other image in the game answers for itself. Measured across traced boots, the sequence falls from about 5.3 seconds to under 0.3. The install refuses unless the body it is about to hook really does zero the field the address pack names, which is what ties the pack to the executable in front of it rather than to the one it was derived from.

**The opening movie** is skipped by reproducing a path the engine already has. On Ayesha the routine that opens a movie begins by asking whether the movie subsystem is ready, and when the answer is no it writes 1 to the player's state byte and returns without opening anything; the per-frame update reads that byte first and reports "not playing", so the caller advances as though the movie had finished. On the other two the play routine has a matching exit for a movie whose file does not open, which the shipped games can reach today because the regional movie directories are not all complete. In both cases the skip returns without calling the original, which lands the engine in exactly that state.

**It counts plays rather than identifying movies**, and that is what keeps the in-game Movies gallery working. Every movie a game plays goes through the same routine, so a rule keyed on which movie it is cannot tell the opening at boot from the opening the player just chose from the menu. The budget is one movie: it is spent during boot, and everything afterwards plays normally. A count also retires a question static analysis could not settle, since Ayesha's table carries both an opening and a pre-title sequence with no constant-index call site saying which one boots. Every index the routine is asked for is still logged once, so a run says what booted even though nothing depends on it.

Every build has independently derived addresses and verified prologues, and the field offsets were read out of each build rather than carried over. `DUSK_SKIP_LOGOS` and `DUSK_SKIP_INTRO_MOVIE` control the two for one session; `[Startup] SkipLogos` and `[Startup] SkipIntroMovie` are the ini keys, named to match the Arland mod's.

## Startup window background

Between the game's window appearing and its first frame being drawn there is about a second in which Windows fills the window with mid-grey, because that is the brush the game's window class carries. The mod asks for black instead, which is what the game fades up from.

The correction intercepts one call and changes one field of one window class, and only when three conditions hold together: the class comes from the executable itself, it is named `KTGL.A11`, and its background is exactly the grey stock brush. Windows hands out one shared handle per stock brush for the whole process, so that last comparison is an exact match rather than a guess. Anything else passes through untouched, so a build that stopped asking for grey would simply stop being affected. No game code is patched, no address is hardcoded, and the structure the game passed is never written to: the swap is made on a copy.

Ayesha registers the same class name as the Arland games and with the same brush. Escha & Logy and Shallie do not contain that class name, so the correction never matches there and needs no per-game gate to say so. Always on, with no setting, and confirmed in game.

## Launcher

The custom launcher is a 64-bit Win32 settings program. It edits the game's `Setting.ini` for resolution, fullscreen mode, language, and outlines, and `dusk-fix.ini` for launcher and rendering state. Auto is represented in `dusk-fix.ini` and resolved to a literal desktop resolution in `Setting.ini` when saved.

It carries three tabs — General, Graphics, and About — laid out to match the Arland mod's launcher row for row, so someone who has used one does not have to learn the other. A setting the running game does not have is not shown: the window reads the same per-game capability list the DLL does, and a game without a given feature gets no control for it. Saving never writes a key for a feature the running game would ignore.

Every write to either file is checked. A failure is verified against the file before it is reported, because the flush call reports failure under Wine even when the values reached disk, and warning about a save that did in fact happen is worse than the failure. Real failures name the Win32 reason and are written to `dusk-fix.log`; misreported ones are logged as such and no warning is shown.

The 32-bit `msimg32.dll` proxy is loaded by each game's stock launcher and settings editor. It forwards `AlphaBlend` and `TransparentBlt`. Only the three per-game stock launcher processes are redirected, and only when the custom launcher is installed or `SkipLauncher` selects a game executable. The proxy patches the host entry point in memory, preserves the original bytes for fallback, and keeps the Steam-launched process alive while its child runs.

Layout and DPI scaling, save round-trips into both ini files, the stock-tool buttons, and the redirect chain are confirmed in game.

## Runtime safety

The mod never modifies the game files. Game hooks are gated on executable identity, `.text` size, and complete expected prologues before MinHook is called. Unknown builds receive only normal D3D11 forwarding. Reverse-engineered memory access uses guarded range checks, and all patches, trampolines, snapshots, and cached pointers disappear with the process.

A last-chance exception filter writes a crash log with module and offset resolution for each stack frame, so a failure during a diagnostic session is identifiable without a debugger.

## Provenance

Philip Rebohle created the original `atelier-sync-fix` synchronization implementation. TellowKrinkle's fork supplied prior Ayesha Map/Unmap work and the old-Arland rendering correction. The Dusk atlas, field, antialiasing, save, and animation measurements, address mapping, and integration are this project's work. Atelier Graphics Tweak is prior behavioural evidence only; none of its code is included. MinHook is by Tsuda Kageyu and contributors.
