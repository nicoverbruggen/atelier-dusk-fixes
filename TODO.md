# TODO

This file tracks work in progress and ideas under consideration.

## Live

- **Run the Ayesha atlas diagnostic.** Build, deploy `d3d11.dll` next to the
  game executable, launch with `DUSK_ATLAS_STATS=1` on the English build, open
  Status and Synthesis, and read `dusk-fix.log`. This decides whether the Arland
  atlas cache is worth porting and, if so, which lifetime it needs (queue-scoped
  as Totori/Meruru, or frame-scoped as Rorona). Everything below it is blocked on
  this. See `TECHNICAL.md` §1.8 and §2.
- After that run: port the atlas cache if the numbers justify it, or record the
  negative result in `TECHNICAL.md` and close the item.

## Investigation

- ~~Ayesha DX: locate the queue-drain / text-renderer / atlas lock–unlock
  homologs.~~ Done — both builds mapped and corroborated, `TECHNICAL.md` §1.7.
- ~~Determine whether Escha & Logy and Shallie share the menu-hitch pattern.~~
  Done — they do not; their text-rendering layer diverged and only the shared
  middleware lock stub survives (`TECHNICAL.md` §1.3). Menu work is Ayesha-only.
- Fingerprint the remaining four Dusk executables (Escha EN + ML, Shallie EN +
  ML): SHA-256, `.text` sizes, CRT generation, launcher relationship. Ayesha's
  two are done.
- Escha & Logy DX: investigate the shadow-texture problem AGT's "eschatology
  hack" worked around (SRV replacement at init); decide whether a proper fix is
  feasible. This is the Dusk item with the clearest unmet user need — the
  community ReShade workaround is unmaintained and reported to crash.
- Shallie DX: investigate the `CreateSamplerState` bug AGT patched.
- Determine which sync-fix lineage applies: upstream `atelier-sync-fix` targets
  these newer engine revisions directly — measure whether the Arland-style
  per-resource deferred-upload refinement is needed or upstream suffices.
- Decide the shipping shape: one `d3d11.dll` per trilogy, or extend the
  launcher/resolution work too (the Dusk launchers are structurally similar;
  verify).

## Notes for whoever picks this up

- Ayesha is an incremental-link build: `callsites` on a real function reports
  only its jump thunk. Re-run `callsites` on the thunk address. This has already
  produced one wrong-looking negative during the mapping above — see
  `TECHNICAL.md` §1.4.
