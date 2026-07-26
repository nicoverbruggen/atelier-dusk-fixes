# TODO

This file tracks work in progress and ideas under consideration.

## Live

- **Glyph-check the atlas cache before release.** It now ships ON by default, so
  this is the one gate left between it and users. Play through and confirm **no
  glyph is wrong or missing** — the cache's failure mode is corrupted text.
  Include text the atlas has to page in; uncommon kanji on the multilingual build
  is what caught the Arland version out, and is the case this port has never
  exercised. `DUSK_ATLAS_CACHE=0` is the escape hatch if a report arrives.
  See `TECHNICAL.md` §4.2 for the residual risk being carried.
- **Reduce out-of-drain snapshot churn (best remaining win).** Steady state does
  27 real atlas reads per frame where 3 should do, each a ~1 MB snapshot rebuild,
  costing ~3 ms of a ~4 ms frame (`TECHNICAL.md` §2.5). Understand the write-lock
  pairing first — the 2:1 write-to-read ratio is the probable cause and is still
  unexplained. Do **not** simply relax the unmatched-unlock invalidation: that rule
  is what stops stale glyphs, and glyph correctness is unvalidated (§4.2).
- **Fix the main-menu per-frame re-render** (smaller than it looked: only ~6% of
  steady-state text cost is above the atlas, §2.5). Measured but unaddressed: the main
  menu re-renders two unchanged strings every frame for as long as it is open
  (`TECHNICAL.md` §2.3). Needs the Arland text-bitmap replay cache, but
  `BalloonBucMode` is the wrong scope signal for a menu (§3) — the open question
  is what scope to key it on, not how to write the cache.

- **Validate the field-jitter port (Ayesha).** First confirm the symptom exists:
  run at a high refresh rate and stand on a step or ledge. Then
  `DUSK_FIELD_TRACE=1` to confirm the controller offsets read as plausible values
  on this build, then `DUSK_FIELD_ENGINE_FIX=1`, and only then add
  `DUSK_FIELD_STABILIZER=1`. The stabilizer writes into live game state at
  unconfirmed offsets, so the trace step is not optional (`TECHNICAL.md` §5).

## Investigation

- ~~Locate Ayesha's queue-drain / text-renderer / atlas lock–unlock homologs.~~
  Done, both builds, corroborated (`TECHNICAL.md` §1.7).
- ~~Determine whether Escha & Logy and Shallie share the menu-hitch pattern.~~
  Done — they do not (§1.3). Menu work is Ayesha-only.
- ~~Decide the atlas cache lifetime.~~ Done by measurement: frame-scoped, because
  72% of candidate locks fall outside the queue drain (§2.3).
- ~~Add out-of-drain and Present-to-Present timing to the diagnostic.~~ Done:
  reports now carry `frameMicros`, `lockMicros`, `renderMicros` and
  `aboveAtlasMicros` (§2.4). Not yet run — a `DUSK_ATLAS_STATS=1` session is what
  sizes the main-menu re-render fix and says how much the cache left behind.
- Explain the 2:1 write-to-read lock ratio (§2.1). No longer cosmetic: it is the
  probable cause of the snapshot churn above, because the cache's LIFO
  lock/unlock pairing assumes Arland's one-write-plus-one-read per glyph.
- ~~Resolve the `BalloonBucMode` destructor.~~ Done: `0x20cdc0` EN / `0x211b90`
  ML, resolved from vtable cross-references after the homologue vote came back
  MISMATCH, with the method validated against Meruru first (§3).
- Port the Meruru conversation fix for Ayesha's field conversations. Both ctor and
  dtor are now verified, so this is a direct port. Unmeasured though — no run so
  far had a conversation on screen, so confirm the symptom first.
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

- Ayesha is an incremental-link build: a call-site search on a real function
  reports only its jump thunk. Search the thunk address instead. This has already
  produced one wrong-looking negative — see `TECHNICAL.md` §1.4.
- Ayesha creates its swap chain via `D3D11CreateDevice` +
  `IDXGIFactory::CreateSwapChain`, not `D3D11CreateDeviceAndSwapChain`. Hooking
  only the latter silently costs you the frame boundary, and the resulting log
  looks like a measurement rather than a gap (§2.2).
