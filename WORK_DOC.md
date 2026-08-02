# Work document

The working technical and investigation record for this early-stage mod. Keep
it in sync with the code and with new runtime or binary-analysis evidence. It
may contain shipped measurements, work in progress, open questions and
unvalidated ports while the first version is being developed. `TECHNICAL.md` is
the separate release-facing summary of finalized, measured behaviour; update it
only when evidence is ready to be presented as a release guarantee.

This project is much earlier than its Arland sibling. One fix ships — the Ayesha
font-atlas read cache — plus the diagnostics that justified it. The opt-in
switches listed in the README beyond those are experimental and are not
documented as established behaviour.

Two larger pieces are implemented but unvalidated, and both are one in-game
session away from an answer: the **launcher** (the window, the 32-bit redirect
and the `dusk-fix.ini` layer behind them) and the **high-resolution fix** for
Ayesha. The current state of each, including exactly what the next run has to
show, is under "The launcher window" and "The high-resolution fix" →
"Validation status". `../atelier-re-tools/DUSK.md` carries the same two as its
top queue items.

Steam app IDs: Ayesha `1152300`, Escha & Logy `1152310`, Shallie `1152320`.

## Historical background

This repository combines the established Atelier synchronization lineage with
new Dusk-specific research. The components should not be conflated:

- Philip Rebohle created [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) in 2022. Its proxy loading and MinHook-based native D3D11 interception are the foundation of `src/core/main.cpp`, as they are of the Arland project's.
- [TellowKrinkle's fork](https://github.com/TellowKrinkle/atelier-sync-fix) added Map/Unmap shadow coherence specifically for Atelier Ayesha, plus old-Arland render-target and viewport/scissor correction. That fork is why the modding community already groups Ayesha with the Arland render family, and it is independent corroboration of this project's own finding that Ayesha shares the Arland text path.
- Nico Verbruggen, the author of this repository, carried out the Dusk-specific reverse-engineering: the three-way engine triage that established which Dusk games share the Arland menu-hitch code, the Ayesha address packs, and the measurements that selected the atlas cache's lifetime. That work was carried out with the assistance of large language models, and it reuses mechanisms proven in the Arland project rather than inventing new ones.
- Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) targeted these titles directly (SMAA, a resolution hack, a withdrawn anti-stutter, an Escha & Logy shadow-texture fix, and a Shallie sampler-state patch). It is important prior work; none of its code is used here.
- [MinHook](https://github.com/TsudaKageyu/minhook) is an independent library by Tsuda Kageyu and contributors, bundled unchanged under `vendor/minhook`.

## Two engines, one DLL

> **TL;DR**: The three Dusk games are not one target. Ayesha is built on the same engine generation as the Arland games and inherits their menu problem; Escha & Logy and Shallie are a newer engine that does not have it. The source is split along that line, but it still builds into a single file that users drop into any of the three games.

The Dusk DX ports share a lineage but not a codebase. Ayesha is the
PhyreEngine-derived, old-MSVC-CRT build whose font-atlas and text-rendering code
is the same code, function for function, as the one behind the Arland menu
hitch. Escha & Logy and Shallie are UCRT builds on the newer LTGL/KTGL engine,
with fast menus and a text layer that has no homologue of that path at all. That
is a measured conclusion, not an assumption from release dates; the evidence is
under "The engine triage" below.

The source tree follows the engine boundary:

```
src/core/     engine-agnostic: the D3D11 proxy, engine dispatch, the capability
              matrix, the ini layer, the high-resolution fix and its census,
              hook installation, logging
src/engines/phyre/  Ayesha — the atlas read cache
src/engines/ktgl/   Escha & Logy and Shallie — fingerprinting only, so far
src/launcher/ neither engine, and one of the two is not even the same
              architecture: the launcher window and the 32-bit msimg32 proxy
```

Neither engine module includes the other's headers, and no address pack lives in
`src/core`. Nothing either module knows is meaningful in the other's process.

They still ship as **one `d3d11.dll`**. Every fix is gated twice, on the
capability matrix in `core/game.cpp` and on an executable fingerprint, so a
module loaded into the wrong process installs nothing; splitting the artifact as
well would buy no safety and cost the user a per-game download.
`core/engine.cpp` resolves the engine from the executable name once and forwards
initialization and the frame tick to that module only.

`src/engines/ktgl/` implements no fix yet, so all it does is establish identity: it
fingerprints the four Escha & Logy and Shallie builds exactly as the Phyre module
fingerprints Ayesha's two, and logs the `.text` size it saw. That is the gate any
future fix for those games will install behind, and logging the size means a game
patch that changes `.text` shows up in the log rather than being silently
accepted.

## The engine triage

> **TL;DR**: Before porting the Arland menu fix, the question was whether the Dusk games contain the same code at all. Ayesha does, exactly; Escha & Logy and Shallie do not, so the menu work is Ayesha-only and no amount of configuration will apply it to them.

The Arland menu-hitch fix hooks four game-side entry points per executable — the
resource-event **queue drain**, the **text renderer**, and the middleware
**atlas lock** and **atlas unlock**. The question for Dusk was whether those four
exist in the Dusk executables, and in the same relationship to each other.

They were located by static homologue matching: shared exact byte n-grams voted
per `.pdata` function, verified in both directions, plus prologue comparison, run
from all three Arland English builds into each Dusk executable. This is the same
technique that located the Arland multilingual entry points. Weak verdicts were
corroborated by independent caller-shape comparison rather than accepted on the
vote alone.

**Ayesha reproduces the pattern exactly.** All three Arland games vote
independently to the same four Ayesha addresses, and the prologues are
byte-identical to their Arland counterparts:

| Anchor | Rorona EN → | Totori EN → | Meruru EN → | Verdict |
|---|---|---|---|---|
| Queue drain | `0x78320` | `0x78320` | `0x78320` | MATCH (12/10/7 votes, reverse CONFIRMS) |
| Text renderer | `0x74bd90` | `0x74bd90` | `0x74bd90` | MATCH (28/28/37 votes, runner-up 0) |
| Atlas lock | `0x581420` | `0x581420` | `0x581420` | WEAK vote, corroborated below |
| Atlas unlock (impl) | `0x581470` | — | — | MATCH (27 votes) |

**Escha & Logy and Shallie do not.** The text renderer produces *no n-gram
matches at all* in either binary, and the queue drain mismatches on both vote and
prologue shape:

| Anchor | Escha EN | Shallie EN |
|---|---|---|
| Text renderer | no n-gram matches | no n-gram matches |
| Queue drain | WEAK, prologue shape DIFFERS (`0x293850`) | MISMATCH — do not trust |
| Atlas lock | `0x3f1dd0`, byte-identical stub, 6 direct callers | `0x3c8ef0`, byte-identical stub |

The shared *middleware* lock stub survives in all three games — it is
byte-for-byte the same 0x19-byte function everywhere — but the text-rendering
layer above it diverged. Escha's stub has 6 direct call sites against Ayesha's
11, and none of Escha's callers is a text-renderer homologue. Escha & Logy and
Shallie therefore carry no known menu hitch and are not targets for this fix.

### Corroborating the atlas lock past its WEAK verdict

The lock is a 0x19-byte leaf, so only one n-gram is usable and the matcher cannot
do better than WEAK regardless of correctness. Four independent checks agree.
Rorona, Totori and Meruru all vote to `0x581420`, and every reverse vote
confirms. The prologue and size are byte-identical to all three Arland locks,
with the same `0x19` extent and the same `.pdata` bounds. The body is identical
instruction-for-instruction to Meruru's, modulo the call displacement, which in
Ayesha targets a thunk (`0x17b7f` → `0x580f90`) where Meruru calls the
implementation directly. And, the load-bearing one, resolved through the thunk
Ayesha has **11 call sites, exactly matching Meruru's 11**, one of which
(`0x74c020`) lies inside `0x74bd90`, the independently MATCH-verified text
renderer. The lock is called from the text renderer, in the same relationship the
fix depends on.

### Ayesha is an incremental-link build

Ayesha's `.text` opens with an incremental-link jump-thunk table (`e9 <rel32>`
entries at `0x1000`–`0x18000`, outside `.pdata`). Calls do not reach their target
directly; they call a thunk that jumps to the real function. Neither Arland build
does this.

The practical consequence is a trap for any call-site search: querying a real
function finds only its thunk, and the function looks nearly unused. The atlas
lock resolves this way — a direct search reports one unverified `jmp` from
`0x3abc`, while searching `0x3abc` itself reports the 11 genuine call sites
above. The thunk table sits at identical RVAs in the English and multilingual
builds (`0x3abc` reaches the atlas lock in both), which is itself a useful
anchor.

### The unlock has two levels; the stub is hooked

Ayesha exposes both shapes the Arland project encountered:

```
0x581460   mov r8d, edx ; xor edx, edx ; jmp 0x136b   -> thunk -> 0x581470
0x581470   the 0x159-byte implementation
```

`0x581460` is the exact homologue of Meruru's hooked unlock (`0x3ea7f0`,
byte-identical shape); `0x581470` is the homologue of Rorona's (`0x3eea60`,
matching `0x159` size). The Arland project hooks the stub on Totori and Meruru
and the implementation on Rorona.

Coverage is equivalent — the implementation is reachable only through the stub,
whose sole user is `0x581465` — so the choice is about argument convention. This
repository hooks the stub, so the hook sees the same `(rcx, rdx=0, r8d=mode)`
convention as the existing Meruru/Totori path rather than Rorona's. Its callers
resolve through thunk `0x1163`: 14 sites, including `0x74c09c` inside the text
renderer, pairing with the lock's `0x74c020`. Because the stub's 16-byte window
contains the `jmp rel32` displacement, it needs a per-build expected array,
following the Arland `dtorExpectedEn/Multi` precedent.

## Repeated font-atlas reads

> **TL;DR**: Ayesha's menus are slow to open because the game re-reads its three font textures thousands of times while building a menu, and again every frame while one is on screen. The mod takes one copy per texture per frame and serves the repeats from it. Menu construction fell from 248 ms to 38 ms.

The static mapping proved the *code path* exists in the same shape. On its own it
did not prove the hitch has the same *cause* in Ayesha: how many candidate reads
occur, whether they are redundant, what they cost, and whether Ayesha's drain
brackets menu construction the way Totori's and Meruru's do were all open. The
Arland investigation repeatedly disproved plausible static hypotheses with a
single instrumented run, so these were measured rather than assumed — and the
answers were not the ones the static picture suggested.

### The pattern reproduces, and Ayesha is worse than any Arland game

| Drain | Wall time | renderText | Candidate locks | read / write | Distinct textures |
|---|---:|---:|---:|---:|---:|
| #658 | 39.2 ms | 18 | 342 | 114 / 228 | 3 |
| #762 | 266.7 ms | 262 | 2385 | 795 / 1590 | 3 |
| #943 | 248.4 ms | 262 | 2385 | 795 / 1590 | 3 |

The dimension offsets hold: 100% of candidate locks report 512×512, so the
`+0x40/+0x42` reads carried over from the Arland middleware are correct in Ayesha
and the counts are trustworthy. Those 2385 locks land on 3 distinct middleware
textures — the Arland shape exactly, where Totori made 2,331 candidate reads
against 3 atlases and Meruru 3,030 — so the same collapse to three real reads is
available. The cost is larger here: Arland's worst comparable drains were
82–117 ms against Ayesha's 248–267 ms, and #762 and #943 are the same operation
repeated with byte-identical counts, which is the signature of work that does not
depend on changing state. Locks and unlocks balance exactly
(2385 + 262 = 2647), confirming the unlock stub hook observes the same population
as the lock hook.

### The lifetime is frame-scoped

An early run reported no out-of-drain locks at all, which looks like evidence for
the queue-scoped (Totori/Meruru) lifetime. It was not: Ayesha reaches its swap
chain through `D3D11CreateDevice` followed by `IDXGIFactory::CreateSwapChain`,
not `D3D11CreateDeviceAndSwapChain`, so hooking only the latter left Present
unhooked and the out-of-drain counters were never flushed. Both routes are hooked
now, and with the frame boundary firing the picture is the opposite:

| Scope | Frames / drains | Candidate locks | Share |
|---|---:|---:|---:|
| Out of drain | 95 frames | 18,876 | 72% |
| In drain | 4 drains | 7,497 | 28% |

Most font-atlas traffic happens outside the queue drain, so **Ayesha needs
Rorona's frame-scoped lifetime**; the queue-scoped one would miss the majority of
the work. The in-drain figures reproduced exactly across both runs — three drains
at 2385 candidate locks and 248 ms — so that operation is deterministic and the
two sessions are comparable on it.

### The cache

The cache ships **on by default** on Ayesha, and is hard-off for Escha & Logy and
Shallie. Its structure follows the Arland implementation with the frame-scoped
lifetime. The queue drain deliberately neither arms nor clears it; Present owns
both, so snapshots survive across drains within a frame and never across a frame
boundary.

The rules are carried over from Arland, each for a reason that was paid for
there. A lock is eligible only while the verified text renderer is on the stack,
with a real output pointer, on a 512×512 middleware texture. **Snapshots are
created only from read-mode locks**, because a write mapping is discard-mapped
and its contents undefined on entry; snapshotting one captures uninitialized
memory and poisons the read that follows, and since the first candidate lock of
each atlas is a write, this is the difference between working and a corrupted
glyph. **Both modes are still served** from an existing snapshot, because writer
and reader name the same middleware object, so one snapshot stands in for the
whole rasterize-then-read-back round trip; serving reads only would forfeit most
of the saving. Synthetic locks are tracked per thread and each holds its snapshot
alive by `shared_ptr`, so a clear or invalidation on another thread cannot free a
buffer a caller is still reading, and only a matching synthetic unlock is
suppressed. **Any unmatched real unlock invalidates that texture's snapshot**,
because the glyph atlas is a single mutable, demand-paged surface and without
this a glyph paged in after the snapshot is served stale and blits blank — the
Arland missing-kanji bug. Hooks arm behaviour only after all four install, so a
partial install is pass-through rather than half-caching.

English build, same operations with the cache off and on:

| Menu build | Cache off | Cache on | Reduction |
|---|---:|---:|---:|
| 2385 candidate locks | 248.4 ms | 38.2 ms | 85% |
| 1527 candidate locks | — | 24.1 ms | — |
| 342 candidate locks | 39.0 ms | 8.8 ms | 78% |

Session totals: 63,517 cache hits against 3,029 real reads, a 95.5% hit rate. For
comparison the Arland cache reduced its equivalent drains by 34–48%; the larger
gain here reflects Ayesha's greater redundancy, not a better cache. Sitting in
the main menu, the steady-state frame interval fell from ~12.3 ms to ~4 ms.

The residual ~38 ms of a menu build is CPU-side glyph and layout construction
above the atlas, which this cache cannot reach — the same conclusion the Arland
project reached by measurement there.

### Open main-menu replay-cache investigation

Ayesha rebuilds two unchanged main-menu strings from scratch every frame. A
representative steady-state frame measured 4,646 µs total, with 3,889 µs inside
two `renderText` calls and 132 atlas locks. A text-bitmap replay cache that
returns the already-built output for an unchanged key would skip those calls and
remove about 84% of the frame, taking the atlas locks generated by them to zero.
The earlier estimate of roughly 6% counted only work above the atlas inside
`renderText` and treated the atlas traffic as irreducible; it was the wrong
estimate for a replay cache that bypasses the entire renderer.

The mechanism already exists in the Arland project as `cachedRenderText`, keyed
on renderer, font, atlas, style and exact string. The unresolved part is the
lifetime boundary. Meruru widens its replay cache only while a `BalloonBucMode`
conversation balloon is live. Ayesha's main menu is not a balloon, and making
the replay cache permanent is not acceptable until invalidation is complete for
the menu lifetime.

Verified Ayesha `BalloonBucMode` entry points, useful for a separate conversation
port but not as the main-menu scope:

| Entry point | Ayesha EN | Ayesha multilingual |
|---|---:|---:|
| constructor | `0x20cc60` | `0x211a30` |
| destructor | `0x20cdc0` | `0x211b90` |

The constructor is a homologue MATCH. The destructor was resolved from class
vtable references because its compiler register choice breaks body matching;
both are verified `.pdata` starts with matching sizes across builds.

The next step is to identify a main-menu-specific activation or generation
boundary in both builds, confirm that the two steady-state `renderText` calls
belong to it, and capture their exact strings and keys. An initial opt-in port
must preserve executable and prologue gating and be tested across menu entry,
state and language changes, and exit/re-entry. Measure it with atlas statistics
both disabled and enabled because the diagnostic mutex and maps inflate absolute
hook timings.

Separate lead: determine whether Ayesha field conversations reproduce Meruru's
repeated-balloon render. If they do, the verified `BalloonBucMode` hooks make
that a direct but separately scoped port.

On the read side the cache is at its theoretical floor. In a steady-state frame
with the main menu open it performs exactly **three real read locks, one per
atlas**, which is the minimum a frame-scoped lifetime permits, and its
invalidation rule never fires (`snapshotDrops=0` across 147 reported frames).
The real locks that remain in such a frame are write mappings, which a cache of
read snapshots cannot serve: a write mapping is discard-mapped, so its contents
are undefined on entry and it can never be the source of a snapshot.

### Why serving a stale glyph is not possible

The cache's one failure mode is serving a snapshot after something else has
written the atlas. That was closed by enumeration rather than by playtesting,
because "we did not see it happen" is not an argument.

**Every unmap routes through the hooked unlock.** The unlock implementation
(`0x581470` EN) has exactly one entry, thunk `0x136b`, whose only user is
`0x581465` inside the hooked stub. There is no path that releases an atlas
mapping without the mod seeing it, so the invalidation rule cannot be bypassed.

**Only two callers touch an atlas, on one thread.** A census keyed on (caller
RVA, thread, access mode) over every 512×512 lock — deliberately *not* filtered
by the cache's own eligibility rule, so that it would see the callers that rule
excludes — recorded 41,368 locks across ~10,500 frames of the multilingual
build. Three callers appeared, all on a single thread:

| Caller (ML) | Function | Mode | Locks | Textures |
|---|---|---|---:|---|
| `0x5cc029` | `0x5cbfe0`, the write-begin helper | 3, write | 27,568 | the 3 atlases |
| `0x76e525` | `0x76e290`, `renderText` | 0, read | 13,784 | the 3 atlases |
| `0x63c78b` | `0x63c680` (EN `0x61a180`) | 3, write | 16 | 16 others, no atlas |

The third caller is one of the nine lock callers that are not reachable from
`renderText`; it touches sixteen unrelated 512×512 textures once each and never
an atlas.

The write-to-read ratio also resolves here: 27,568 / 13,784 is exactly 2.000, and
it comes from **one** write call site invoked twice per read, not from two
different writers.

**Nothing writes an atlas behind the middleware's back.** A probe on
`ID3D11DeviceContext::Map`, `UpdateSubresource`, `CopyResource` and
`CopySubresourceRegion` recorded 4,908 writes to 512×512 textures from exactly
two call sites, `0x5a3576` and `0x5a3746`. Both lie inside `0x5a3490`, which is
the atlas lock's own implementation: the hooked lock `0x5a3920` calls thunk
`0x151db`, which jumps to `0x5a3490`. Every D3D11-level write to a 512×512
texture therefore originated inside the code the mod already intercepts.

**Single-threadedness is what makes this sufficient.** The cache serves only
while the text renderer is on the stack *of the calling thread*. With all atlas
traffic on one thread there is no concurrency, so a write from outside the text
renderer cannot execute during a serve; it can only run between serves, and its
unlock then invalidates the snapshot before the next one. The argument does not
depend on having enumerated every possible writer — it holds even if an
unobserved caller exists.

### Risk accepted in shipping it on by default

The failure mode is wrong or missing glyphs, and it has **not** been validated by
a playthrough. The decision to ship it on is deliberate.

The untested case is a glyph the atlas must page in mid-frame. The invalidation
rule exists for it, and it is the rule that fixed the equivalent Arland
missing-kanji bug, so the design anticipates the case; what is missing is
confirmation at runtime. The multilingual build is the higher-risk one, since
uncommon kanji page in far more often than Latin. If a report arrives,
`DUSK_ATLAS_CACHE=0` restores the game's own behaviour without a rebuild, and the
log records the cache's hit/real-read split for any run.

## SMAA

These games ship no antialiasing of any kind. SMAA (Jimenez et al.) is a
post-process over the finished image, so unlike MSAA it smooths any visible edge
regardless of how it was produced, including texture-interior and alpha-test
edges. It is `OptIn` on Ayesha and `Unsupported` on the other two.

### What was ported, and from where

The passes are the Arland project's `src/smaa.cpp`, which is this project's own
code (MIT), ported into `src/core/smaa.cpp` with the Arland-specific parts
removed: no MSAA twin write-back, because that does not exist here, and
initially no supersampling stand-in either -- `smaaApplySceneColor` has since
grown into that role and is what the supersampling downscale calls. The
reference shader and
the `AreaTex`/`SearchTex` lookup tables are vendored unchanged under
`vendor/smaa/` (MIT) and compiled at runtime through `d3dcompiler`, at
`SMAA_PRESET_ULTRA`. The wrapper around the reference entry points is Arland's
own, not the SMAA distribution's DX10 sample.

Two other mods were checked first, and the survey is worth recording because it
changed the plan:

- **TellowKrinkle's rendering branch has no SMAA at all.** It is MSAA,
  sample-rate shading, anisotropic filtering and LOD bias. Nothing to take.
- **Atelier Graphics Tweak does ship SMAA for these games**, and inspecting it
  (behaviour only; it is unlicensed and none of its code is used) settled two
  questions. Its AA is stock: the same MIT `SMAA.hlsl` this project now vendors,
  plus the SMAA distribution's own DX10 wrapper, at `SMAA_PRESET_ULTRA` /
  `SMAA_HLSL_4_1`, compiled at runtime from a bundled `d3dcompiler_47.dll`. And
  its injection point is on the **deferred** context, from its log strings
  (`deferred:PSSetShader(tonemapShaderNoAA)`): it hooks `CreatePixelShader`,
  recognises a post-process composite shader, substitutes a bundled no-AA
  variant, and injects when that shader is set.

  Its shader substitution does **not** target Ayesha. The two `.dxbc`
  replacements sample `smplScene`, `smplZ`, `smplBlurFront`,
  `smplAdaptedLumCur`, `smplDOFMerge`, `smplBloom`, `smplStar`, `smplFlare` and
  `smplLightShaftLinWork2`, and none of the 139 DXBC shaders in Ayesha's
  `Res/x64/cg/commonShader.PSSG` contains any of those names. They belong to
  other games in AGT's list. So the technique transfers and the identification
  does not.

The one genuinely transferable fact is the deferred context, and it agrees with
what this project already learned the hard way: Ayesha draws on a deferred
context, which is what defeated the first two attempts at the high-resolution
raster correction. `highres.cpp` already hooks both context vtables, including
all four draw entry points.

### Why it runs at Present, and why that is temporary

`smaaApply` runs in the Present hook, over the swap chain's back buffer. That
needs no engine knowledge at all, which is the point of doing it first: it is
the half of the feature that could be landed and looked at without an
investigation.

It is also the half with a real cost. The frame at Present is fully composited,
so the passes antialias the interface and its text along with the scene. Arland
avoids that by injecting on the scene target before UI composition, and finding
that boundary was a separate piece of work for each Arland title: Rorona and
Meruru use "the first draw into the main target with depth testing disabled",
and Totori needed a draw-state trace before it could key on "same targets
retained, depth test disabled". Ayesha will need its own, and the existing
deferred-context draw hooks are where it would be found.

That is why SMAA is `OptIn` while every other shipping fix here is on by
default. The others have no trade-off to weigh; this one currently does.

**The two paragraphs above are the record of why the Present path was written
first, not a description of where SMAA runs today.** The scene/UI boundary was
subsequently found -- this engine composites its interface onto the back buffer
after the composite, so a scene colour target simply ceasing to be the render
target *is* the boundary, observed rather than heuristically guessed the way
each Arland title needed. `smaaApplySceneColor` is the pre-UI pass, and
`SMAA: pre-UI active size=` is validated in game with text staying crisp.

There are now three placements, and only one runs in any given session:

| Configuration | Where SMAA runs | Size it runs at |
|---|---|---|
| SMAA on, supersampling off | `msaaNoteSceneBoundary`, on the scene colour host | display |
| SMAA on, supersampling on | inside the supersampling downscale, on the display-sized result | display |
| pre-UI never claimed a frame | `smaaApply` at Present, over the finished frame | display |

Boundary SMAA stands down entirely whenever supersampling is configured. Two
pre-UI passes would antialias the scene twice, and the boundary one is keyed on
a transition that fires 5-22 times a frame -- see "Supersampling", "SMAA
placement", for the rest of that argument. If supersampling is configured but
never engages, the pre-UI path claims no frame and SMAA falls back to the
Present line above, which announces itself.

### Diagnostics

`DUSK_SMAA_DEBUG=1` replaces the frame with the edge-detection output (red
horizontal, green vertical) over a dimmed scene, `=2` with the amplified blend
weights. A black debug view means the pass produced nothing, which separates a
lookup-texture problem from a blending one. Environment-only, like every other
diagnostic here.

Any failure -- no `d3dcompiler`, a shader that will not compile, a resource that
will not create -- logs once, sets the broken flag and leaves SMAA off for the
rest of the session without touching anything else.

### Shipped on by default, 2026-08-02

SMAA is now `OnByDefault` on Ayesha. That became defensible only when the pre-UI
injection landed: the Present-time path antialiased the composited frame,
interface included, and softened menu text badly enough that it was switched off
after its first validation. Running on the scene target before the interface is
composited leaves text untouched.

Two consequences worth stating:

- The Present path still exists as a fallback and stands down permanently once
  the pre-UI path has proven itself once. Before that -- logos and menus, where
  no scene target exists yet -- it still runs, so the first few seconds of a
  session do get their text softened.
- With supersampling on, SMAA runs **inside** the downscale pass, on the
  display-sized result, rather than at the boundary on the scene target. Cheaper
  (a quarter of the pixels at 200%), correct scale for a morphological filter,
  and it reuses a state bracket that already exists. The boundary path stands
  down only once supersampling has actually *engaged*, not merely when it is
  configured -- otherwise a configured-but-never-attached supersampling setting
  would leave SMAA with nowhere to run and fall back to softening the interface.

## MSAA and supersampling

Added alongside SMAA so the antialiasing options sit together. All three are
`OptIn` on Ayesha and `Unsupported` on the KTGL games, and all three carry ini
keys and launcher controls, unlike every other fix in this project: each trades
frame rate for image quality, and which trade to take is exactly the judgement a
setting exists for. Supersampling is the most expensive thing this mod can do
and can never be a default; the launcher presets put it only in the two top
rungs.

Both of these sections were rewritten after their first versions shipped
conclusions that a runtime measurement then contradicted. That is worth noting
at the top rather than burying: in both cases the mistake was reasoning from
what the game **creates** to what the game **uses**. Supersampling then went on
to be rewritten four times, three of them for ordering bugs, which is why its
section leads with the ordering rule rather than with the mechanism.

### MSAA: the engine will not do it, so the mod does

**The first version of this feature was wrong, and the way it was wrong is the
useful part of this record.**

It raised `SampleDesc.Count` on targets the engine had already created
multisampled, on the strength of a census line: Ayesha creates scene colour
(bindFlags 0x28) and scene depth (0x48) at `SampleDesc.Count == 4`. The
conclusion drawn was "the engine already multisamples its 3D scene at 4x, so
this is just a bump". The census reports the targets a game **creates**.
Creation is not use, and nothing in that reasoning ever checked use.

Draw-time instrumentation settled it. Counting every sampled draw by the sample
count of the target it landed on, over 7200 frames of ordinary play:

```
draws=1791745  drawsTo1x=117245  drawsToMsaa=0  maxBoundSamples=0
```

Not one draw ever bound a multisampled target. The engine creates six of them at
startup -- colour+depth pairs at 1920x1080, 1728x972 and 1536x864, exactly
100/90/80% -- and renders into none of them. The old implementation was raising
the sample count on dead allocations, and reported `action=msaa` while doing it.

(The counter samples the first draw after each raster change, not every draw,
so 117245 of 1791745 is a biased sample rather than a census. A scene pass sets
a viewport before drawing, so it is caught; `maxBoundSamples` never left 0.)

#### Why the engine cannot be asked

Ayesha's texture-creation wrapper at `0x559600` takes an anti-aliasing **level
index** as its seventh argument, looks the sample count up in a table, and
stores it into the descriptor:

```
0x55965c  movsxd rcx, dword ptr [rbp+0x4f]        ; arg7 = AA level
0x559672  mov eax, dword ptr [rax + rcx*4]        ; rax = 0xfe8418
0x559675  mov dword ptr [rbp-0x65], eax           ; -> SampleDesc.Count
0x559678  mov eax, dword ptr [rbx + rcx*4 + 0x3098]
0x559682  mov dword ptr [rbp-0x61], eax           ; -> SampleDesc.Quality
```

The table at RVA `0xfe8418` holds `1, 2, 4, 0`: a sample-count ladder indexed by
AA level. So the renderer does have an MSAA path, and a complete one: `0x5575e0`
loops AA levels 0..2, calls `ID3D11Device::CheckMultisampleQualityLevels`
(vtable slot **30**, offset `0xf0`) across 125 formats, and stores the result
into the very quality array the wrapper reads:

```
0x005575f9  lea r14, [rcx + 0x3098]                 ; &renderer[0x3098]
0x00557620  mov rcx, qword ptr [r15 + 0x3070]       ; the device
0x00557635  mov edx, dword ptr [r12 + rax*8 + 0xfe7c00]   ; a format
0x0055763d  call qword ptr [r10 + 0xf0]             ; CheckMultisampleQualityLevels
0x0055765e  mov dword ptr [r14], ebx                ; -> +0x3098 / +0x309c / +0x30a0
0x00557667  cmp esi, 3
```

This corrects an earlier reading in this project's notes that the engine never
probes multisample capability and leaves the quality array permanently zero.
That reading came from searching vtable slot 27 (`0xd8`) for the call, which is
`CreateDeferredContext`; and from `writes-to-offset ayesha-en 0x3098` reporting
only the constructor's zeroing. Both were artefacts. The engine's MSAA plumbing
is not half-wired — it is complete and simply never selected for the scene.

**A tooling blind spot worth knowing** (also noted in `RE-PLAYBOOK.md`):
`writes-to-offset` matches a literal `[reg+0xNNN]` operand, so it cannot see the
store above. The offset was hoisted into `r14` by a single `lea` and then
advanced by `add r14, 4` per iteration, leaving the actual store encoded as
`mov dword ptr [r14], ebx` with no displacement at all. This is a third blind
spot alongside wide SIMD stores (which is how `+0x305c` was missed) and calls
reached through incremental-link thunks.

It is not reachable. The wrapper has eight call sites, reached through the jmp
thunk at `0x25400`. **Six pass a literal zero** (verified individually, e.g.
`0x55c601 mov dword ptr [rsp+0x30], r8d` with `r8d` zeroed at `0x55c570`; same
shape at `0x55e948`, `0x5622dd`, `0x559edf`, `0x559fec`, `0x55f514`). Only two
forward a variable, and that chain resolves to a renderer field `+0x305c`, read
at `0x56740e`/`0x567492` in `0x567280` -- which is what produces the six dead
allocations. The scene's own colour and depth pair comes from a hardcoded-zero
site.

No ini key, registry value or command line reaches any function on those paths.
Ayesha's only config reader (`0x71a550`) handles `ScreenWidth`/`ScreenHeight`/
fullscreen and nothing else. **There is no setting to flip.**

Two threads were left open and are recorded rather than closed: what table
`0x566500` walks to set `+0x305c` (it is written by a wide SIMD store at
`0x566f00` that the offset scanner cannot see), and one branch that dead-ends at
an unresolved virtual call at `0x55bc90`. Neither affects the conclusion, which
rests on the hardcoded-zero sites.

A claim made during this investigation and then **withdrawn**: that Ayesha's
shipped antialiasing is FXAA and that its tonemap pass samples scene depth for
distant blur. That came from Atelier Graphics Tweak's bundled shader reflection
(`smplZ`, `fDistantBlurZThreshold`, `patch_FXAA applied`). Those belong to
another game in AGT's list -- none of Ayesha's 139 DXBC shaders in
`Res/x64/cg/commonShader.PSSG` contains those names, as this document already
recorded elsewhere. Nothing is currently established about what antialiasing
Ayesha ships with.

#### The twin implementation

`src/core/msaa.cpp`. The mechanism is TellowKrinkle's by way of the Arland
project's `src/sync_fix.cpp`: the game keeps its single-sample colour and depth
targets ("hosts"), the mod attaches a multisample "twin" to each as private
data, substitutes the twins when that pair is bound, and lands the colour twin
back into its host before anything reads it.

Interception points. The detours live in `msaa.cpp`; the vtables they go into
are owned by `src/core/d3d11_hooks.cpp`, which is the only module in the tree
that calls MinHook on a D3D11 vtable. (That module was extracted from
`highres.cpp` during this work: the resolution fix had ended up hosting five
detours belonging to a feature it has nothing to do with, and each group is now
installed only when its own feature is on.) Slot numbers are enumerated from the
MinGW `d3d11.h` this cross-build uses, never counted by hand:

| Slot | Method | Role |
|---|---|---|
| 33 | `OMSetRenderTargets` | substitute twins; land the displaced pair |
| 8 | `PSSetShaderResources` | resolve before the game samples a host |
| 47 | `CopyResource` | resolve before a copy reads a host |
| 46 | `CopySubresourceRegion` | same |
| 114 | `FinishCommandList` | resolve before a recorded list is closed |
| 58 | `ExecuteCommandList` | resolve before a replay clobbers this context |
| 50 | `ClearRenderTargetView` | clear the twin, not the host |
| 53 | `ClearDepthStencilView` | clear the depth twin, not the host |
| 34 | `OMSetRenderTargetsAndUnorderedAccessViews` | the other bind route |
| 22 | `CreateRasterizerState` (device) | force `MultisampleEnable` |

The two clears were **missing from the first version of this implementation**,
and their absence is worth recording because it is the same failure shape as the
original defect. The game clears the view it created -- the host -- while the
twin is what is bound and drawn into. An unintercepted clear therefore zeroes a
surface nobody renders to and leaves the twin holding the previous frame, so
every frame composites on top of the last, while every counter in the module
reports success. It was found by diffing the hook table against the two
reference implementations rather than by reasoning, which is the lesson:
`sync_fix.cpp:2928/2942/3513` and `impl.cpp:1564/1575/1745` all hook these, and
checking that list is cheaper than deriving it.

**Eligibility is split across the engine boundary, deliberately.** Core asks
only whether a twin *can* exist -- single-sample, one mip, one array slice, and
a sample count both formats support -- because those are properties of the D3D11
mechanism and hold anywhere. Which pair is *the scene* is a `MsaaSceneTest`
callback supplied by an engine module, and core declines every bind (with one
logged line saying so) until one is registered.

That boundary is not decoration. "Which of the several dozen binds a frame
issues carries the 3D scene" is a renderer property, and the prior art proves it
varies: TellowKrinkle counts more than 64 indexed draws into a target,
the Arland project matches the main render size *and* a BGRA view format.
Getting it wrong is quiet -- it multisamples a shadow map, leaves the scene
untouched, and every counter still reports success, which is precisely the
failure this rewrite exists to stop repeating.

`src/engines/phyre/scene_target.cpp` supplies Ayesha's rule. It began as "colour
and depth both at exactly the main render size" and **a run proved that
insufficient**: at 1440p the main render size equals the swap-chain size, so the
back buffer paired with the swap-size depth matched it as well as the real scene
pair did, and the composite/UI pass was being multisampled and resolved on every
bind. Measured descriptors:

```
pair #1  colour format=87 (B8G8R8A8_UNORM)    bind=0x28 | depth bind=0x40   back buffer
pair #2  colour format=90 (B8G8R8A8_TYPELESS) bind=0x28 | depth bind=0x48   scene
pair #3  colour format=90 (B8G8R8A8_TYPELESS) bind=0x28 | depth bind=0x48   scene
```

Two discriminators were added, each stating something true about what a scene
target *is* on this engine rather than merely separating these surfaces: the
colour target is **typeless** (this engine allocates surfaces it will both
render into and sample back as typeless, so it can put typed RTV and SRV over
one allocation; the back buffer is a presented surface and is typed), and the
depth target carries **SHADER_RESOURCE** (the scene's depth is allocated
readable; the composite pass's is not).

What this deliberately does not do is exclude the back buffer by identity. A
renderer that draws its scene straight into the back buffer is an ordinary
design -- the Arland games do exactly that -- so "never twin the back buffer"
would be wrong as a general rule and wrong in `src/core`.

Pairs #2 and #3 have identical descriptors and are **both** the scene: the
engine ping-pongs between two same-shaped colour targets for its post-processing
chain, which is why the over-match warning now fires on a pair of a different
*shape* rather than on a second pair. Counting alone cried wolf on the normal
case while saying nothing about what differed. It is reliable here *because of* the defect
`highres.cpp` corrects -- this engine pins its scene targets to a hard-coded
1920x1080 while everything else it allocates is another shape. A censused
session settles at 15 distinct shapes, of which exactly one colour+depth pair is
at the main size; the shadow map is 1024x1024, the blur chain runs 960x540 down
to 64x36, the UI composites at swap-chain size. It is also the same rule the
resolution fix already uses to decide what to enlarge, and that fix is validated
in game at 1440p and 4K -- so the rule is not new evidence, it is evidence
already paid for. The format is deliberately left unconstrained: Ayesha renders
to an offscreen pair rather than to the back buffer, so the size test already
isolates it and a format condition would narrow the rule on no evidence.

It registers from `initializePhyreFixes()` **before** that function's feature
gate, because it needs no hooks, no mapped addresses and no fingerprint, and a
session that enables only MSAA must still get it.

Escha & Logy and Shallie register nothing, which is the honest state: their
renderer has never been censused, so nothing is known about what they bind.

Requiring a depth target at all matters for a separate reason: a multisample
colour target bound with a single-sample depth target is invalid.

Two things this does that the prior art does not:

- **Sample counts are walked down per format** against
  `CheckMultisampleQualityLevels`, taking the lower of what colour and depth
  support, so an 8x request on a device that refuses it becomes 4x rather than
  nothing.
- **Every resolve saves, drops and restores the render-target bindings.** A twin
  still bound as a render target is not a legal resolve source. Arland and
  TellowKrinkle both resolve without unbinding; two of the resolve points here
  fire mid-pass with the scene targets legitimately bound, so an unbind that was
  not put back would leave the rest of that pass writing nowhere.

`CreateRasterizerState` is not optional. Multisampled targets with
`MultisampleEnable` clear receive single-sample coverage, and every counter in
the module would still report success -- the exact failure this rewrite exists
to stop repeating. If that hook cannot be installed, MSAA declines to run.

#### The open question: depth

`ResolveSubresource` rejects depth-stencil formats, so a depth twin can never be
landed back into its host. Only colour hosts are ever marked dirty here, so the
invalid resolve is never attempted.

Both prior-art implementations have the same property, but by accident rather
than by design: neither ever marks a depth resource dirty, so neither has ever
tried it -- and neither could have detected it if they had, because
`ResolveSubresource` returns void and a rejected call surfaces only through the
D3D11 debug layer.
TellowKrinkle additionally strips `SHADER_RESOURCE` off the depth twin
(`impl.cpp:311`), which suggests the game he targeted does create its depth with
that flag -- as Ayesha does (bindFlags `0x48`).

Whether that costs anything here depends on whether this engine ever samples its
scene depth host, which is **not established** (see the withdrawn evidence
above). So it is measured rather than argued: the depth host is tagged at twin
time and SRV binds of it are counted as `depthHostReads`. Non-zero means a
shader-based depth-resolve pass has to be written (a fullscreen pass reading
`Texture2DMS<float>` and writing `SV_Depth`, modelled on Arland's
`ScopedSmaaState` discipline, which additionally needs scissor-rect save/restore
that Arland's version omits). Zero means there is nothing to write.

#### Audited, and what was deliberately left alone

The implementation was audited against both reference implementations after it
was written, because the feature it replaces was wrong in a way no counter
caught. Five defects were found and fixed:

- **A re-bind of the already-substituted pair marked the host clean while the
  twin kept accumulating.** The bind detour lands the displaced pair after
  `msaaSubstituteTargets` has re-marked the arriving one dirty, so when the two
  are the same pair -- a redundant re-bind of the scene targets, which nothing
  forbids an engine -- the resolve ran, marked the host clean, and handed the
  twin straight back for further drawing. Every later read of the host that
  frame would then skip its resolve and read a stale surface, with every
  counter reporting success. Arland is immune by ordering: its
  `resolveBoundMSAA` runs before the `Dirty` store re-marks the host. Here the
  pending entry is dropped instead when it names the pair being re-substituted,
  since nothing moved off the twin and there is nothing to land.

- **The twin was undefined on the frame it was created.** Twins are built lazily
  on the first *bind*, but the engine clears before it binds -- so that frame's
  clear went to the host while no twin existed to redirect it to, and the twin
  was drawn into with `CreateTexture2D`'s undefined contents. One visibly bad
  frame as MSAA engages, self-healing after. Twins are now cleared at creation.
- The three missing hooks (`ClearRenderTargetView`, `ClearDepthStencilView`,
  `OMSetRenderTargetsAndUnorderedAccessViews`) described above.
- SMAA's internal binds went through the public `OMSetRenderTargets` and so
  re-entered the MSAA substitution logic for binds it was never meant to see.
  Harmless today only because Ayesha's immediate context holds no scene marker
  at Present -- a property of one engine, not a guarantee. All internal binds
  now go through `d3d11SetRenderTargets`.
- `msaaResolveBeforePresent` was documented as a safety net. For Ayesha it is
  almost certainly a no-op, because `FinishCommandList` has already resolved and
  cleared the deferred context's markers. The comment now says so.

Four further findings were judged **not worth building for**, and are recorded
here so the decision is visible rather than looking like an oversight:

- **UAVs are not saved across a resolve.** `resolveColor` captures and restores
  render targets via `OMSetRenderTargets`, which per D3D11 semantics unbinds any
  UAVs bound above the render-target count. A mid-pass resolve would drop them.
  Nothing indicates this engine's scene pass binds UAVs, and covering it means
  moving to `OMGetRenderTargetsAndUnorderedAccessViews` and carrying UAV state
  through every resolve. Revisit if a KTGL renderer turns out to use them.
- **SMAA holds no device identity.** Its shared resources are built once and
  never checked against the device that later uses them, so a device reset would
  bind objects from a destroyed device. Inherited from the Arland implementation,
  which has the identical gap; a pre-existing risk in both projects rather than
  something this port introduced.
- **SMAA restores no state after its passes**, on the reasoning that it runs last
  before Present. That is true of the current frame but assumes the engine
  re-specifies everything on the next one, which was never measured. The symptom
  would be one corrupted frame after each SMAA frame, so a run answers it.
- **A command list replayed more than once** would replay the resolve recorded
  into it, copying whatever the twin holds at replay time. No evidence Ayesha
  reuses command lists.

Three things were traced and found correct, which is worth recording because
they are the parts most likely to be wrong: reference counting across every
failure branch of `msaaSubstituteTargets` and `resolveColor`; deferred-context
marker isolation (per-instance private data partitions naturally, and
`FinishCommandList` clears both slots before returning); and the absence of
concurrent writes to the module's globals.

One diagnostic was added rather than a fix: the scene test matches any
colour+depth pair at the main render size, and a wrongly matched pair would look
identical to a correct one in every counter. A second distinct pair being
twinned now logs `MSAA: WARNING a second distinct target pair matched the scene
test`.

#### What the log says

`FIXES msaa=` reports only that MSAA is **configured**. `MSAA: engaged` is a
separate line emitted the first time a bind is actually substituted, and a
periodic `MSAA twinPairs=... substitutions=... colorResolves=...
depthHostReads=... declinedBinds=... twinFailures=...` line carries the rest.
Conflating "configured" with "engaged" is how the previous implementation went
unnoticed, so the two are deliberately different lines.

The silent complement is named too: a one-shot `MSAA: configured but nothing
engaged after 1800 frames` line fires when a scene test is registered and
declined every bind for 30 seconds. One reachable cause is `DUSK_HIGHRES=0`,
which stands down the CreateTexture2D hook that learns the main render size, so
Ayesha's scene test can never match (see the note in
`src/engines/phyre/scene_target.cpp`).

**Status: built and deployed, not yet validated in game.**

### Supersampling: rebuilt on a positively identified composite

**Four implementations preceded this one and none of them worked.** The list is
the useful part of this record, because three of the four failed on ordering and
the fourth on an optimisation that contradicted a finding made minutes earlier.

| # | Design | Outcome |
|---|---|---|
| 1 | Back-buffer redirect (the Arland design, ported): substitute a larger texture when the game creates an RTV over the back buffer, average it down at Present | **Black screen.** Ayesha created exactly one such view, at startup, and never composited through it. The scene is never in the back buffer here. `redirects=1` was the only evidence. |
| 2 | Enlarge the scene targets and let the engine's own composite resample them | Worked, but the engine samples through a bilinear sampler -- four taps, which only resamples correctly at a whole-number ratio. It also **silently disabled MSAA**, because "how big are the scene targets" was written down in both the resize and the MSAA scene test and the two disagreed the moment this was switched on. |
| 3 | Own the downscale: a ratio-sized box filter (later with a sharpen folded in) substituted at the composite's sample | "Better but not much." It ran on every scene-to-non-scene transition, of which there are 5-22 per frame. |
| 4 | Add a once-per-frame latch to (3) | **Black 3D scene, missing interface.** The first transition each frame is a post-processing bind, not the composite, so the composite was handed a copy of a half-finished scene. |

The lesson, which the rebuild is built on: **"the engine stopped rendering into
the scene target" is not "the composite is about to run".** It fires many times
a frame while the post-processing chain steps in and out of the two ping-ponged
scene colour targets. The composite has to be identified positively.

#### The mechanism

`highres.cpp` already rewrites this engine's pinned 1920x1080 scene targets to
the main render size and carries the hard-coded viewport and scissor with them
-- machinery validated in game at 1440p and 4K. Supersampling multiplies that
one size. The engine then renders and post-processes at the enlarged size,
untouched, and the mod intervenes at exactly one moment:

1. The swap chain's back buffer is tagged at creation (`ssaaNoteBackBuffer`,
   called from both swap-chain routes in `main.cpp`). Identity by **tag**, not
   by a held reference (which would block `ResizeBuffers`) and not by raw
   pointer (a freed address gets handed back out). `ssaaFrameTick` re-verifies
   the tag once a frame and re-applies it if a resize dropped it.
2. A bind whose colour render target carries that tag **is** the composite, and
   nothing else in this renderer is. `ssaaNoteTargetsBound` sets a per-context
   marker while it is bound and clears it otherwise.
3. While that marker is set, the moment the engine binds a scene colour host as
   a pixel-shader resource is the composite's sample -- and **that sample is the
   resample**. `ssaaSubstituteShaderResources` box-filters the host down to
   display size, runs SMAA on the result if it is enabled, and substitutes a
   view over the display-sized copy **in the forwarded argument array of that
   one call**.
4. Nothing is latched across the call, so a post-processing pass sampling the
   same texture later cannot inherit the substitution -- which is what made
   attempt 4's picture black.
5. **Nothing at all happens at Present** except counters. That is the design's
   safety argument, not an optimisation: two of the four predecessors blacked
   the screen out with a present-time pass painting over a finished frame, and
   there is no longer such a pass to get wrong.

Substituting at the **sample** rather than at the **bind** is also what resolves
the ping-pong: the engine has two byte-identical scene colour targets, and only
the view the composite actually binds says which one it reads.

If identification never fires, the player sees attempt 2's picture -- the
engine's own bilinear downscale of the enlarged scene, soft but correct -- and
the log names that state rather than leaving it to be inferred from a counter
that stays at zero.

#### Ordering, which is a correctness requirement

Three of the four failures were ordering bugs. The invariant is one sentence:
**anything that reads a scene colour host must run after `msaaResolveReplaced`,
because under MSAA the scene is in the multisample twin until that call lands
it.**

`hookedOMSetRenderTargets`, top to bottom (mirrored in
`hookedOMSetRenderTargetsAndUnorderedAccessViews`, which leaves all four steps
alone on the `D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL` sentinel):

1. `msaaSubstituteTargets` -- decide the twins, set the displaced pair aside
2. `msaaResolveReplaced` -- land the displaced twin into its host
   *(everything below reads the host)*
3. `msaaNoteSceneBoundary` -- tag arriving scene colour hosts; fire pre-UI SMAA
   only when supersampling is off
4. `ssaaNoteTargetsBound` -- set or clear the composite marker
5. forward the bind

`hookedPSSetShaderResources`:

1. `msaaResolveShaderResources` -- the hosts become current
2. `ssaaSubstituteShaderResources` -- only with the marker set and the re-entry
   guard clear
3. forward the possibly-substituted array

`hookedFinishCommandList`: `msaaResolveBeforeFinish`, then
`ssaaClearContextState` -- a list can be closed with the back buffer still
bound, and the marker is per-context state the next recording must not inherit.

#### Two rules inside the pass

**Every internal bind goes through a trampoline.** Attempt 4 called
`context->OMSetRenderTargets` inside its pass, re-entered the MSAA detour that
hook lives in, and hung the game on the loading screen before the intro video
could play. `Draw`, `RSSetViewports`, `RSSetScissorRects`, `PSSetShaderResources`
and `OMSetRenderTargets` are all hooked in this project; the pass reaches every
one of them through `d3d11OriginalsFor()` / `d3d11SetRenderTargets`.

**A thread-local in-pass flag guards re-entry.** The pass calls SMAA, which
binds shader resources through the public method; without the guard that bind
walks straight back into `ssaaSubstituteShaderResources` with the composite
marker still set.

The state bracket (`ScopedPassState`) saves more than the pass itself touches:
16 pixel-shader resource slots, 4 samplers, 4 constant buffers per stage, the IA
state, raster, viewports, scissors, blend, depth-stencil, both shaders and the
render targets. SMAA runs inside this bracket and binds up to ten shader
resources while restoring only four, so the six it would leave behind are
covered here rather than by editing a feature that is already validated in game.
Restore order matches SMAA's and the engine's own: render targets first, shader
resources last.

#### One owner of every shared fact

The second attempt disabled MSAA for a whole session because two places computed
the scene size and drifted apart. So:

| Fact | Sole owner |
|---|---|
| Main render size (display) | `highResMainSize()` |
| Scene-target size (main x factor, clamped to 7680x4320, even) | `highResSceneSize()` -- consumed by the `CreateTexture2D` resize, the resize gate, the blur target (scene/2) and the MSAA scene test |
| The supersampling factor and its clamp arithmetic | `ssaaPercent()` / `ssaaSceneSize()` |
| Which colour+depth pair is the scene | `MsaaSceneTest` (`phyreSceneTargets`); `msaaNoteSceneBoundary` is the only writer of the scene-host tag |
| Which resource is the back buffer | the private-data tag from `ssaaNoteBackBuffer` |

#### SMAA placement

After the downscale, at display resolution, inside the downscale pass's state
bracket. Three reasons, first one decisive: SMAA is a morphological filter whose
search distances are counted in pixels, so on a 2x scene target every edge is
twice as wide as its thresholds expect. It is also a quarter of the work at
2560x1440 that it is at 5120x2880, including the full-surface copy it starts
with, and it reuses a bracket that is already held. It stays pre-UI by
construction, because this engine draws its interface onto the back buffer after
the composite.

Boundary SMAA is therefore **suppressed** whenever supersampling is configured.
If supersampling never engages, SMAA degrades to the Present path and its own
one-shot says so.

#### The log contract

Every mechanism has to distinguish *configured*, *attached* and *doing
something*, and each silent failure has to name itself. The maintainer verifies
by reading logs.

```
FIXES ssaa=off                                            -> CONFIGURED (off)
FIXES ssaa=200% scene=5120x2880 display=2560x1440 sharpen=35%
      ('SSAA: composite identified' confirms attachment)  -> CONFIGURED (on)
HIGHRES: scene size 5120x2880 = main 2560x1440 x 200%     -> the single size fact
SSAA: back buffer identified 2560x1440 format=87          -> identity anchor exists
SSAA: composite identified -- bind whose colour target is the swap-chain back
      buffer (frame N)                                    -> ATTACHED
SSAA: engaged -- 5120x2880 -> 2560x1440 substituted at the composite's sample
      (slot S)                                            -> DOING SOMETHING
SSAA compositeBinds=A sceneSrvSubstitutions=B downscales=C passFailures=D
```

The periodic line is emitted every 600 frames. `A` should be about one per
frame. `B == 0` with `A > 0` falsifies risk 1 below.

The named negative states, each a one-shot:

- `SSAA: configured but the composite was never identified after 1800 frames`
- `SSAA: composite identified but no scene target was ever sampled during it`
- `SSAA: high-resolution fix is off; supersampling requires it and is inactive`
- `SSAA: the downscale pass could not be prepared ...`
- `SSAA: the back buffer lost its tag and was re-identified (a swap-chain resize)`
- `D3D11HOOKS: WARNING the high-resolution fix is off ...` -- the `DUSK_HIGHRES=0`
  trap, which kills MSAA and supersampling together and has cost a session before

The `FIXES ssaa=` on-line is emitted from `ssaaFrameTick` rather than from
`d3d11_hooks.cpp` where the `off` line lives, and that split is deliberate:
neither size exists at install time, because Ayesha creates its device before
its swap chain and the main render size is learned from the first depth target
after that. The alternative was a configured line reading `scene=unknown`.

Under supersampling the existing `MSAA: engaged ... size=` must show the SCENE
size and `SMAA: pre-UI active size=` the DISPLAY size. That cross-check lives in
one log and is the cheapest way to confirm the single-owner rule held.

#### Configuration

`[Rendering] Supersampling` as an integer percentage: 125, 150, 200, 300, 400,
anything else off. A percentage rather than a decimal because `1.5` in an ini is
a locale trap -- a comma-decimal locale parses it as `1`.
`[Rendering] SupersamplingSharpen` is the post-downscale unsharp amount, also a
percentage, default 35. `DUSK_SSAA` and `DUSK_SSAA_SHARPEN` override both.

`Feature::Supersampling` is `OptIn` on Ayesha and `Unsupported` on the KTGL
games, and its `Descriptor` is **env-only despite the ini key existing**: the key
holds an integer, and `featureEnabled`'s boolean path would seed the literal
`false` into it the first time anything asked whether the feature was on.
`ssaaPercent()` owns that key; the capability matrix still owns whether the
running game supports the feature at all.

It cannot ever be a default. 200% is four times the shaded pixels, measured at
**70% GPU on a 7900 XTX in the game's opening interior** -- close to the lightest
scene in the game. With 4x MSAA that is sixteen geometry samples per displayed
pixel.

#### Risks carried, and what falsifies each

1. **The composite may set its shader resources before it binds the back
   buffer.** Falsified by `compositeBinds > 0` with `sceneSrvSubstitutions = 0`,
   which has its own one-shot line. Contingency: on the marker being set, call
   `PSGetShaderResources(0..15)` and re-set any slot carrying the scene-host tag.
2. **Post-processing at enlarged sizes.** Largely cleared by an independent scan
   of all 139 DXBC containers in `Res/x64/cg/commonShader.PSSG`: no
   screen/viewport/target-size constant exists anywhere (the only name in that
   family is `TexelSize`, a per-draw `$Params` float4 in two vertex shaders), and
   no hard-coded resolution literal appears in any `SHDR`/`SHEX` chunk scanned as
   a float32 stream. Attempt 2 additionally ran the full chain at 200% and the
   image worked. Residual: fractional factors and the `scene/2` blur rounding.
   Falsified by misplaced bloom at 150%.
3. **A full-screen pass inside a `PSSetShaderResources` detour disturbs the
   composite's setup.** Mitigated by the wide state bracket and trampoline-only
   binds. This is the part with the least evidence behind it.
4. **Back-buffer tag staleness after a buffer recreate.** Per-frame re-verify.
5. **A UI pass sampling a scene host would get the substitute.** Normalised UVs
   make it size-transparent. Accepted.
6. **Cost.** See above; opt-in, and the launcher presets put supersampling only
   in the two top rungs.

A seventh is worth writing down because it is untested rather than reasoned
about: at **exactly 1080p with supersampling on**, the engine's swap-chain-sized
targets are the same 1920x1080 the resize matches on, so a swap-sized target
created after the main one is adopted would be enlarged along with the scene
targets. Above 1080p the two sizes differ and the question does not arise. The
test rig is 1440p, so this has never been exercised.

#### Fractional factors

Supported: `ceil(ratio)` taps per axis with a half-ratio backoff, each tap
bilinear, which lands on texel centres for odd ratios as well as even ones. The
restriction to whole-number factors belonged to attempt 2, where the ENGINE
owned the resample; it outlived its reason by several builds and silently
refused the setting the ini asked for. **150% is restored but unproven on this
engine** -- no fractional factor has ever been run here.

#### Depth, and a qualification on "no depth resolve needed"

The MSAA work concluded that no depth-resolve pass is needed, on a run that
measured `depthHostReads=0`. That measurement stands for the scenes tested, but
the shader-bundle scan found `0x54718` -- a **DOF + glow composite** declaring
four textures (`texture_`, `backBufferTexture`, **`BaseDepth`**, `SoftColor`)
with `$Globals.DofParams`. It samples a full-screen colour texture and a depth
texture in the same pass. Either it samples a copy rather than the scene depth
host, or it is simply not used in those scenes.

**Keep the `depthHostReads` counter and keep reading it.** If it ever goes
non-zero the depth twin is stale for that pass and a shader-based depth resolve
becomes real work again. The question is not closed.

(That same shader is the strongest candidate for the final full-screen
composite, which corroborates the identification strategy above but does not
replace it: the mod identifies the composite by the back buffer being bound as a
render target, which is a runtime fact and needs no shader knowledge.)

#### Validation runs still owed

Nothing below has been run. **Do not treat any of this as validated.**

1. Supersampling off: regression guard -- `MSAA: engaged`, `twinPairs=2`,
   `SMAA: pre-UI active`.
2. `DUSK_SSAA=200`, MSAA off, SMAA off: the four SSAA lines in order; a sharper
   image; the interface intact.
3. 200% + MSAA 4x: `MSAA: engaged ... size=<scene>`; `depthHostReads=0`; ~70% GPU.
4. 200% + MSAA 4x + SMAA: `SMAA: pre-UI active size=<display>`.
5. `DUSK_SSAA=150`: the open fractional question; watch bloom placement.
6. Launcher: walk the preset ladder, edit a control, reopen.

**Status: built, not deployed, not validated in game.**

#### Validated in game, 2026-08-02

Every rung of the log contract appeared, in order, on the first run:

```
SSAA: back buffer identified 2560x1440 format=87
HIGHRES: scene size 5120x2880 = main 2560x1440 x 200%
SSAA: composite identified -- bind whose colour target is the swap-chain back
      buffer (frame 0)
SSAA: engaged -- 5120x2880 -> 2560x1440 substituted at the composite's sample
```

Counters over a field session: `sceneSrvSubstitutions` and `downscales` both at
exactly one per frame, `passFailures=0`, `secondHostRefusals=0`.

What that settles, item by item:

- **The known risk did not materialise.** The composite binds the back buffer
  *before* setting its shader resources, so the marker is set when the
  substitution looks. The contingency (re-scanning `PSGetShaderResources` on
  marker set) remains designed and deliberately unbuilt.
- **`secondHostRefusals=0`** — the composite reads only one of the two
  ping-pong scene targets. The memo defect the cross-review caught was still a
  real defect; it simply would not have fired in this scene.
- **One downscale per frame.** The per-frame cost problem that the old latch was
  introduced to solve is gone, without pinning the work to the wrong transition.
- **A full-screen pass injected mid-composite-setup does not disturb the
  composite.** `passFailures=0` and the frame is correct. This was the reviewer's
  least-confident area.
- **Fractional factors work.** 150% ran at 3840x2160 -> 2560x1440 with no bloom
  or glow misplacement, which is the whole reason the downscale is mod-owned
  rather than the engine's four-tap bilinear.
- **All three features agree on the scene size.** With MSAA and SMAA also on,
  `MSAA: engaged size=5120x2880` and `SSAA: engaged 5120x2880 -> 2560x1440` in
  the same log. The drift that once silently disabled MSAA is now a one-line
  check.

Cost, measured on a 7900 XTX at 1440p in the game's opening interior: 200% with
4x MSAA sits around 70% GPU; 200% with 8x MSAA reaches 90%. Neither can be a
default, which is what the preset ladder reflects.

**Still open:** `depthHostReads` remains worth reading. The shader scan of
`commonShader.PSSG` found a DOF/glow composite (`0x54718`) declaring `BaseDepth`
alongside `backBufferTexture`, so the engine does have a pass that samples
depth. Every run so far reports zero, but that is a statement about the scenes
tested, not about the engine.

## The Dusk front-ends

> **TL;DR**: Each Dusk game ships the same two 32-bit front-end programs the Arland games do, and they load a DLL the same way, so the Arland launcher redirect ports across almost unchanged. What is different is that the Dusk launchers are per-game files rather than one shared binary.

Every Dusk game folder holds four executables: the 64-bit English and
multilingual game builds, plus two 32-bit front-ends.

| Front-end | What it is | `.text` VirtualSize | Entry-point RVA |
|---|---|---:|---:|
| `Atelier_<Game>Launcher.exe` | what Steam runs | `0x14f4f4` | `0x1216f2` |
| `Atelier_<Game>Env.exe` | the stock settings editor | `0x1b9e20` | `0x137279` |

All six are `IMAGE_FILE_MACHINE_I386` and all six statically import
**MSIMG32.dll**, for `AlphaBlend` and `TransparentBlt`, which is exactly the
arrangement the Arland proxy relies on. Their import sets are otherwise
identical to the Arland front-ends' as well, `ArlandDXEnv.exe` included, down to
the settings editor importing `d3d11.dll` and `dxgi.dll` while the launcher does
not.

The three launchers are separate compiles of one program: identical `.text`
VirtualSize and identical entry-point RVA, but differing `.text` SHA-256, so
per-game strings live inside the section. The three settings editors are the
same story except that Ayesha's and Shallie's `.text` are byte-identical to each
other and Escha's differs.

| Executable | `.text` SHA-256 |
|---|---|
| `Atelier_AyeshaLauncher.exe` | `d8d87625df6e8895e3221ddeabbcea47d55b94162427c306d0c422abb015e533` |
| `Atelier_Escha_and_LogyLauncher.exe` | `e222f8bee6fd20886c698b32ac4105a29df62f68b0d49696aba60c12fa1d8de7` |
| `Atelier_ShallieLauncher.exe` | `56472142cf4e832ba514b51fbcbb98a991cbf4c6531fcf6c9ae4d3ccd1c3cd1a` |
| `Atelier_AyeshaEnv.exe` | `c10bbf867638c8b06ccca4dca62c668ae0e3a7289ebfd695363fbed44ca0d96e` |
| `Atelier_Escha_and_LogyEnv.exe` | `8b1e6cc7a13c29e2ffb610558a137a2736a259e7cfb0555f6800f22748802df4` |
| `Atelier_ShallieEnv.exe` | `c10bbf867638c8b06ccca4dca62c668ae0e3a7289ebfd695363fbed44ca0d96e` |

The practical consequence for the redirect is that the launcher's **file name**
is what identifies the game, where the Arland proxy could not tell its three
apart at all and had to probe the folder for whichever game executable existed.
That is a simplification rather than a complication: the Dusk proxy knows which
title it is in before it looks at the disk, so resolving which build to start is
a two-candidate check against one row rather than a sweep of all three games.

### How the stock launcher picks the game build

The mapping from `[Lang] Language` to executable is read out of the launcher
rather than assumed from the Arland convention it happens to match. The stock
launcher parses `Setting.ini` by hand, splitting each line at `=`, then runs a
**string**-compare chain over the value against the constants `"1"`, `"2"`,
`"3"` and `"4"` at `0x551ea4`, `0x551ea8`, `0x551eac` and `0x551eb0` (ImageBase
`0x400000`), each arm pushing an executable name:

| `Language` | Branch RVA | Executable |
|---|---:|---|
| `1` (Japanese) | `0x2ff8` | `Atelier_Ayesha.exe` (multilingual) |
| `2` (English) | `0x303a` → `0x30dc` | `Atelier_Ayesha_EN.exe` |
| `3` (Simplified Chinese) | `0x3086` | `Atelier_Ayesha.exe` |
| `4` (Traditional Chinese) | `0x30d5` | `Atelier_Ayesha.exe` |
| anything else | `0x30dc` | `Atelier_Ayesha_EN.exe` |

The `"2"` arm and the unrecognized-value arm are the same branch, which is where
the English default comes from. All four names sit consecutively at
`0x551ed8`–`0x551f18`, immediately before the `Setting.ini` and
`Atelier_AyeshaEnv.exe` constants, and the Escha & Logy and Shallie launchers
carry the identical table at the identical file offset (`0x1508a4`) with their
own executable names.

So the convention is the Arland one, default included. The one place the proxy
could have differed is that it originally tested only the value's first
character, where the launcher compares whole strings: `Language=10` would have
read as Japanese instead of falling to English. It now compares whole strings
too.

### Open, low priority: the settings editor and the 64-bit `d3d11.dll`

Note which executable this is about. The **launcher** does not import `d3d11`
at all, so it is unaffected and always has been. The **settings editor**,
`Atelier_<Game>Env.exe`, is a 32-bit process that statically imports
`d3d11.dll`, and the mod installs a 64-bit `d3d11.dll` into the same folder. If
the loader prefers the application directory for that name, a 32-bit process
cannot load it and the editor fails to start.

This is untested and is not a blocker. Proton cannot answer it: the
`WINEDLLOVERRIDES=d3d11=n,b` the test scripts use falls back to the builtin
when the native load fails, which the Windows loader does not do, so a working
editor under Proton would prove nothing. Arland has the identical layout and has
shipped it for a long time without a report, so whatever the answer is, it
applies to both projects equally.

If it does turn out to be real, the fix is to proxy a DLL that no front-end
imports. Ten names qualify (imported by every game executable, by none of the
six front-ends): `d3d9`, `d3dcompiler_43`, `dwmapi`, `dxva2`, `mf`, `mfplat`,
`mfreadwrite`, `steam_api64`, `vcomp140`, `xinput1_3`. Most rule themselves
out -- `steam_api64` is a real file already in the folder, `mf*` is Media
Foundation and Proton substitutes it, `d3d9` is DXVK's under Proton, and
`xinput1_3` is the controller path, which has its own open work item.
**`dwmapi.dll`** is the best of them: a genuine always-present system DLL with a
tiny export surface. The cost is that the mod would no longer own the D3D11
entry points it currently exports, and would have to hook
`d3d11!D3D11CreateDevice` from `DllMain` instead.

## Ayesha's resolution path

> **TL;DR**: Ayesha takes its resolution straight out of its own `Setting.ini` and does not check the value against anything. So raising the resolution needs no injection at all -- a launcher that writes that file is enough. What is still unknown is whether the *renderer* honours the larger size everywhere, which is the defect the Arland project had to fix.

The game reads `[Graphics] ScreenWidth` and `[Graphics] ScreenHeight` from
`Setting.ini` beside the executable, as wide-character
`GetPrivateProfileInt`-shaped reads. Both builds carry the same reader:

| Anchor | Ayesha EN | Derivation |
|---|---:|---|
| resolution reader | `0x71a550` | the only reader of the `ScreenWidth`/`ScreenHeight` string constants |
| its single caller | `0x19bfe0` | via incremental-link thunk `0x1abe0`, one confirmed call site |

`0x71a550` reads the width into its second argument and the height into its
third. The interesting part is what it does with them:

```
0x0071a69e  cmp dword ptr [r14], 0        ; width
0x0071a6a2  jge 0x71a6ab
0x0071a6a4  mov dword ptr [r14], 0x500    ; 1280
...
0x0071a7a6  mov dword ptr [rsi], 0x2d0    ; 720, the same shape for height
```

That is the **entire** validation: a negative value is replaced by the 1280x720
default, and anything else is passed through. There is no clamp against an
upper bound, no snapping to a table of supported modes, and no consultation of
the display's reported mode list. The caller stores the pair straight into its
configuration object at `+0xc` and `+0x10`, alongside the fullscreen flag at
`+0x27`.

Two things follow, and the second is the one that matters.

**The mod does not need a resolution override.** Arland needs one because Koei
Tecmo's settings editor filters its lists through Windows display-mode
reporting, so a resolution the game would accept can be impossible to select;
the Arland launcher works around that by writing its own ini key and having the
64-bit DLL replace the swap-chain request. Here the game's own key already
accepts any value, so a launcher that writes `Setting.ini` gets a higher
resolution with no injection, no hook and no second source of truth for a
setting the game already owns. This repository deliberately does not add a
`DisplayWidth`/`DisplayHeight` key.

**Whether the renderer follows is a separate question, and it is open.** The
old-Arland renderer sizes its backbuffer from the setting but pins a family of
auxiliary render targets to 1920x1080, so raising the resolution there produces
a larger frame with a smaller image inside it, and the Arland project had to
resize those targets and correct the viewport and scissor state to get a genuine
1440p. Ayesha is a different renderer, so nothing about that carries over by
assumption in either direction. A screenshot cannot settle it either: "the UI
looks right" and "the scene targets are the size they should be" are different
claims, and only the second says what would need fixing.

`DUSK_TARGET_CENSUS=1` answered it by enumeration, and one run was enough.

### Measured: Ayesha has the old-Arland defect

A single windowed run of the English build with `ScreenWidth=2560`,
`ScreenHeight=1440` settles it. Two things are confirmed at once, and neither
needed a 1440p display: the game was asked for a resolution larger than the
screen and it simply made one, which is the static reading of `0x71a550`
confirmed at runtime.

```
Swap chain: 2560x1440 format=87 refresh=60/1 windowed
TARGETCENSUS 2560x1440 rel=matchesSwapChain format=44 samples=1 bindFlags=0x40 callerRva=0x559da6
TARGETCENSUS 1920x1080 rel=1920x1080   format=90 samples=1 bindFlags=0x28 callerRva=0x5596d9
TARGETCENSUS 1920x1080 rel=1920x1080   format=90 samples=4 bindFlags=0x28 callerRva=0x5596d9
TARGETCENSUS 1920x1080 rel=1920x1080   format=44 samples=4 bindFlags=0x48 callerRva=0x5596d9
TARGETCENSUS 1728x972  rel=other       format=90 samples=4 bindFlags=0x28 callerRva=0x5596d9
TARGETCENSUS 1536x864  rel=other       format=90 samples=4 bindFlags=0x28 callerRva=0x5596d9
TARGETCENSUS  960x540  rel=other       format=90 samples=1 bindFlags=0x28 callerRva=0x5596d9
TARGETCENSUS  480x272 / 240x136 / 120x68 / 64x36        same caller
TARGETCENSUS 1024x1024 rel=other       format=44 samples=1 bindFlags=0x48 callerRva=0x5596d9
```

Formats are `87` `B8G8R8A8_UNORM` (the swap chain), `90` `B8G8R8A8_TYPELESS`
(colour targets) and `44` `R24G8_TYPELESS` (depth). Bind flags are `0x28`
`SHADER_RESOURCE|RENDER_TARGET`, `0x48` `SHADER_RESOURCE|DEPTH_STENCIL`, `0x40`
`DEPTH_STENCIL`. The whole session settles at 29 creations across 15 distinct
shapes and never grows again, so this is the complete set.

The conclusion is the one the Arland project reached about its own renderer.
**The swap chain and its matching depth follow the setting; everything the scene
is actually drawn into is pinned to 1920x1080.** The scene is rendered at 1080p
and scaled up to the 1440p backbuffer, so selecting a higher resolution today
buys a larger window and no more detail.

Two caller RVAs appear, `0x5596d9` and `0x559da6`, inside `0x559600` and
`0x559c40`. **Neither attributes the size**, and an earlier reading of this that
said the defect "is in `0x559600`" was wrong. Disassembly shows `0x559600` is
the engine's *generic* texture-creation wrapper: it assembles a
`D3D11_TEXTURE2D_DESC` on the stack purely from its own arguments and calls the
device through the vtable slot at `[rbx+0x3358]`. Everything funnels through it,
so the census's caller RVA names the wrapper and says nothing about who chose
1920x1080. `0x559c40` is a second such path, which happens to be the one the
main depth target takes.

Attribution would need one more stack frame, and it turns out not to be
necessary: the fix does not have to know who asked. The census line still
carries the caller because a *third* distinct RVA appearing would mean a
creation path this was never measured against, which is worth knowing.

The pinned family has structure worth noting:

- A **4x MSAA pair** at 1920x1080, colour and depth, plus a non-MSAA pair.
- **1728x972 and 1536x864**, which are 0.9x and 0.8x of 1920x1080. A quality or
  dynamic-resolution ladder derived from the same fixed base, so a fix that
  rewrites only exact-1920x1080 targets would leave these behind at their
  1080p-derived sizes.
- A **halving chain** 960x540, 480x272, 240x136, 120x68, 64x36, which is a
  bloom or downsample pyramid rooted at half of 1080p. Same problem: derived
  sizes, not literal 1920x1080, so they need proportional scaling rather than a
  match-and-replace.
- A **1024x1024** depth appearing about 50 seconds in, which is the shadow map
  and is resolution-independent by design. Leave it alone.

## The high-resolution fix

> **TL;DR**: The scene targets are made to follow the chosen resolution instead of staying at 1080p, and the hard-coded viewport and scissor follow with them. The mechanism is TellowKrinkle's, refined by the Arland project; this is not a new technique.

Implemented in `src/core/highres.cpp`, sharing its `CreateTexture2D` hook with
the census that measured the defect. They are one subsystem: MinHook allows
exactly one hook per target, so they could not be separate installs, and running
them together is what makes "did this actually resize everything" answerable in
one session. `DUSK_HIGHRES`.

### Prior art, and what was taken from where

TellowKrinkle's rendering branch already solves this for this engine family, and
its `CreateTexture2D` comment names Ayesha outright: *"Some older Atelier games
(Rorona, Meruru, Ayesha) always render at 1080p no matter the requested
resolution"*. Its rule is three parts: take the first depth-stencil target's
size as the main render size, rewrite any render or depth target that is exactly
1920x1080 to that size, and override a full-screen 1920x1080 viewport and
scissor with the bound target's real dimensions.

The Arland project ported that and added three refinements, all carried over
here: **shape validation** on the main-target guess, rather than trusting the
first depth-stencil unconditionally; the **half-size blur rule**, because the
engine hard-codes 960x540 for its blur chain and leaving it behind gives a blur
sampled at 1080p proportions over a 1440p scene; and keeping raster state **per
context** rather than globally.

Yuri Hime's Atelier Graphics Tweak also ships a resolution hack for these games.
It is unlicensed, none of its code is used, and it was not consulted for this.

### What the rules are, and why each is narrow

- **Main size**: the first `D3D11_BIND_DEPTH_STENCIL` texture, accepted only if
  it matches the swap chain's size or is 16:9 and at least 1920x1080. The
  ordering is confirmed in Ayesha's own log -- a 2560x1440 depth at 125 ms, the
  first pinned target at 2241 ms -- and the shape check is what stops a game
  that reordered its creations from having some small depth buffer adopted and
  every scene target resized to it.
- **Full targets**: exactly 1920x1080, render or depth, **created empty**. The
  initial-data check keeps a loaded image that happens to be that size out.
- **Blur targets**: exactly 960x540, colour only, typeless BGRA, one mip, one
  array slice, no MSAA. Narrow on purpose, because "half the pinned size" is a
  shape plenty of unrelated textures could share.
- **Only ever scales up.** At or below 1920x1080 nothing is rewritten at all, so
  an ordinary 1080p session takes the path the game shipped with.
- **A refused resize falls back.** If the driver rejects the enlarged
  descriptor, the creation is retried with exactly what the game asked for, so
  the worst case is the unfixed 1080p frame rather than a missing texture.

### The raster correction: two attempts, and a misread in between

The first implementation rewrote the viewport and scissor **eagerly**, inside
`RSSetViewports` and `RSSetScissorRects`, rather than deferring to the next draw
the way TellowKrinkle's fork and the Arland implementation both do. The reason
was cost: deferring means hooking `Draw`, `DrawIndexed`, `DrawInstanced` and
`DrawIndexedInstanced`, the four hottest entry points in the API, for a
correction that applies to a handful of submissions a frame.

The run that followed produced a frame with the scene complete and correctly
proportioned but drawn into about nine-sixteenths of the window, with the UI
detached from it. That was read at the time as the eager rewrite firing on a
pass it should not have, and the correction was rewritten in the deferred,
target-aware form on the strength of that reading.

**The reading was wrong, and the arithmetic says so.** Nine sixteenths is
0.5625, which is 0.75 squared, and 0.75 is 1920/2560. Two stacked passes each
keeping the top-left three quarters produce it exactly: the scene drawn with a
1920x1080 viewport into the resized 2560x1440 target, then composited with a
1920x1080 viewport into the 2560x1440 backbuffer. That is precisely what the
resize alone does when **no** viewport correction happens at all. The eager
version never fired either; both attempts have the same root cause, and the
second was built on a misdiagnosis of the first.

The deferred form is kept regardless, because it is the better design and the
one the prior art uses: `RSSetViewports` and `RSSetScissorRects` record that
raster state changed, and the four draw hooks settle it at the point where the
render target is bound and can be asked how big it is. A single full-screen
viewport or scissor starting at the origin is enlarged only when the surface
bound underneath it is genuinely larger, which is the condition the eager
version assumed rather than checked. Raster state is kept per context, immediate
and deferred, following the Arland implementation.

### Why it did not fire: the wrong context vtable

The deferred version reported `viewportRewrites=0 scissorRewrites=0` with all
six hooks installed. Step counters were added to tell the four possible failures
apart -- hook never runs, runs but state is never dirty, dirty but the bound
target cannot be read, or everything works and the sizes never match -- and an
instrumented 1080p run answered it immediately:

```
HIGHRES: installed fix=1 census=1 contextHooks=6
frame=1500 ... rsViewports=0 rsScissors=0 draws=0 updates=0
```

**Not one of the six context hooks ever ran**, across 1500 frames, while the
device-level `CreateTexture2D` hook on the same run fired normally.

The cause is a property of how MinHook works against COM. MinHook hooks a
function *address*, and the address comes from a vtable -- so hooking the
immediate context's vtable hooks only contexts of that class. D3D11
implementations give deferred contexts a **different class with a different
vtable**, and this engine issues its draws and its raster state on a deferred
context. Every hook was installed correctly, on functions the game never calls.

This also explains something that had looked like corroboration and was not.
`d3d11_probe.cpp` hooks `Map` on the immediate context's vtable and has always
worked, which made the immediate context look like the right one. Texture
uploads do go through it. Drawing does not.

Both vtables are hooked now. A deferred context is created purely to read its
vtable and released immediately; every deferred context the engine makes shares
it, so hooking through ours covers all of them. Each detour dispatches to the
originals belonging to the vtable its context actually came from, decided by
comparing the context's vtable pointer rather than the object identity, since
the engine may hold several deferred contexts.

Two cases are handled explicitly. If an implementation gives both context types
the **same** function for a method, MinHook refuses the second hook on that
address with `MH_ERROR_ALREADY_CREATED`; that is not an error, and the second
vtable's original is pointed at the trampoline the first install produced. And
if the deferred context cannot be created at all, the install **declines
entirely** rather than resizing targets whose raster state it could never
correct.

The installer now logs both vtable pointers and whether they are distinct, so
the question this cost two runs to answer is a line in every log.

### A silent partial install, now impossible

The eager version could reach a genuinely broken state without saying so. Its
context hooks were installed only `if (g_fixEnabled && context)`, and when no
context was available they were quietly skipped -- while the installer still
logged a flat `HIGHRES: installed fix=1`. That is the worst possible
combination: targets resized, raster correction absent, and a log that says
everything is fine.

Two changes close it. The installer names every hook it puts in and reports the
count, and a fix that cannot install its raster correction **declines to install
at all** rather than half-applying itself -- an unfixed 1080p frame is a much
better failure than a resized target drawn with a 1080p viewport. And
`initializeHighRes` now asks the device for its immediate context when none was
handed to it, so the case that triggered the concern does not arise.

### What is deliberately left alone

- The **0.9x and 0.8x ladder** (1728x972, 1536x864). Neither Arland nor
  TellowKrinkle has an equivalent, so there is no proven rule to carry over, and
  inventing a "scale anything that is 1920x1080 times k" rule is exactly the
  kind of fuzzy match that catches unrelated textures. If a run shows the game
  actually rendering into one of these, the census will say so and it becomes a
  second, evidence-backed rule.
- The **deeper pyramid levels** (480x272, 240x136, 120x68, 64x36). These are not
  exact halvings -- 540/2 is 270, not 272 -- so they are alignment-rounded blur
  levels whose absolute size barely matters visually. Arland leaves its
  equivalents alone.
- The **1024x1024 shadow map**, which is resolution-independent by design.

### Validation status

The **resize** is confirmed working at 2560x1440 on the English build: one run
produced `adoptedAsMain` on the 2560x1440 depth target, `resizedFull` on all
four hard-coded 1920x1080 targets, `resizedBlur` on the 960x540 blur target, and
`passthrough` on the ladder, the deeper pyramid levels and the shadow map,
exactly as the rules intend.

The **raster machinery is confirmed working**, and the deferred-vtable
diagnosis is confirmed structurally. A 1080p run of the English build reports
distinct immediate and deferred vtables (`0x...4410` and `0x...3ca0`), both
hooked, and every stage of the chain running:

```
HIGHRES: context vtables immediate=0x6ffffa6c4410 deferred=0x6ffffa6c3ca0 (distinct, both hooked)
HIGHRES: installed fix=1 census=1 contextVtables=2 hooksPerVtable=6
frame=2400 ... rsViewports=13605 rsScissors=13605 draws=4987 updates=4603
              targetLookupFails=0 viewportRewrites=0 scissorRewrites=0
```

`targetLookupFails=0` says `OMGetRenderTargets` works on the deferred context,
which is the one thing about correcting there that was not obvious.

The `HIGHRES RASTER` lines from that run are worth keeping as the baseline,
because they establish what the engine submits when nothing needs correcting:

```
viewport=1920x1080@0,0  boundTarget=1920x1080
viewport=1024x1024@0,0  boundTarget=1024x1024     (the shadow pass)
viewport=960x540@0,0    boundTarget=960x540
viewport=480x272 / 240x136 / 120x68 / 64x36, each matching its own target
```

**Every viewport exactly matches its bound target**, so `viewportRewrites=0` is
the correct result at 1080p rather than another silent failure -- the rule
enlarges only when the bound surface is larger, and here none is. It also
confirms the shadow pass and the whole blur pyramid submit self-consistent
viewports, so they are untouched at any resolution.

### Confirmed at 4K

The correction is confirmed on the English build at 3840x2160. The install is
complete and the main target is adopted at the selected resolution rather than
pinned:

```
Config:   HighResolution = on
HIGHRES: context vtables immediate=0x6ffffa6c4410 deferred=0x6ffffa6c3ca0 (distinct, both hooked)
HIGHRES: installed fix=1 census=0 contextVtables=2 hooksPerVtable=6
Swap chain: 3840x2160 format=87 refresh=144/1 fullscreen
HIGHRES: main render size 3840x2160
```

Compare the same line at 1080p (`main render size 1920x1080`) and with the fix
off at 4K, where the scene targets stayed `rel=1920x1080`. The maintainer
confirmed the frame in game: the picture is visibly sharper and correctly
framed.

That visual result is what settles the raster correction, and it is a stronger
check than it sounds. The failure mode of a resize whose viewport did not follow
is not a subtle one: the scene renders into the top-left quarter of a 4K target
and the rest stays empty. A frame that is correctly framed *and* sharper is only
producible if the viewport and scissor moved with the target.

`Config: HighResolution = on` also confirms the promoted default end to end. The
key had been cleared from `dusk-fix.ini` before the run, and `featureEnabled()`
seeded it back from the capability matrix, so the automatic path is what ran
rather than a leftover opt-in.

Still uncaptured, and bookkeeping rather than a blocker: that run had `census=0`,
so there is no `viewportRewrites` count on the record. One run with
`DUSK_TARGET_CENSUS=1` at 4K would pin the number and the per-target
`action=resizedFull` lines alongside the frame that has already been seen.

### The defect, re-confirmed at 4K with the fix off

A census run of the English build at 3840x2160 with the fix disabled is the
cleanest statement of the problem the fix exists for. The swap chain is 4K and
the engine's own targets are not:

```
HIGHRES: installed fix=0 census=1 contextVtables=0 hooksPerVtable=0
Swap chain: 3840x2160 format=87 refresh=144/1 fullscreen
TARGETCENSUS 3840x2160 rel=matchesSwapChain ... action=passthrough
TARGETCENSUS 1920x1080 rel=1920x1080 ... action=passthrough   (x4)
```

The `rel=1920x1080` classification is the whole finding. At 1080p those same
four targets report `rel=matchesSwapChain`, because at 1080p the pinned size and
the swap-chain size are the same number and the two explanations are
indistinguishable. At 4K they separate, and the targets stay at 1920x1080 while
the swap chain follows the request. The engine renders the scene at 1080p and
lets the presentation stretch it, which is the old-Arland defect exactly.

Note `mainRT=0x0` and `rsViewports=0 draws=0` in that run's periodic lines:
with `fix=0` no context vtable is hooked, so the raster counters have nothing to
count. Only the `CreateTexture2D` census is live. A run with `fix=0` therefore
says nothing about the correction either way, and should not be read as one.

### A pitfall that cost a run: the environment beats the ini

`featureEnabled()` checks a feature's environment variable *before* its ini key,
so that a diagnostic run can override a persisted setting without editing the
user's file. That is right for a diagnostic and actively wrong for a shipping
feature whose switch is unconditionally exported by a wrapper script. The
workspace's `04-run-ayesha.sh` passed `DUSK_HIGHRES="${DUSK_HIGHRES:-0}"` in the
same block as the atlas diagnostics, which pinned the feature off no matter what
`dusk-fix.ini` said; the run above is that bug, not a code fault. The default
has been removed from the script, so the variable now exists only when someone
sets it deliberately. Any future shipping feature that gains an environment
override inherits this hazard.

`logConfiguration()` reported only the three `[Fixes]` keys, which is why the
run looked ambiguous rather than obviously misconfigured. `HighResolution` is
now in that table, so the log states what is in force.

### Why it is on by default

The feature is `OnByDefault` for Ayesha, applied the way Arland applies its own
correction: automatically, whenever the selected resolution is above 1080p. The
trigger is not the capability matrix but the resize rule itself, which acts only
while the learned main render size exceeds the pinned 1920x1080, the same
`mainWidth > 1920 && mainHeight > 1080` test Arland uses. At 1080p and below the
hooks install and every target passes through, so the default costs a 1080p
player nothing and no one has to know the engine pins its targets.

`DUSK_HIGHRES=0` remains as an escape hatch, and matters more here than for the
other fixes: the failure mode is a visibly wrong picture rather than a silent
regression, so a player who hits one needs a way to confirm what they are
looking at. There is no ini key and no launcher control, because picking the
resolution is already the decision this fix depends on.

The promotion was made before the above-1080p run rather than after it, which
was a real risk at the time: an unconfirmed correction shipping on by default,
where a resize whose viewport did not follow would have left the picture worse
than vanilla. The 4K run above closed it, and the order is worth remembering as
something not to repeat rather than as a precedent.

## The launcher proxy

The Arland launcher redirect is ported into `src/launcher/launcher_proxy.cpp`,
building as a 32-bit `msimg32.dll` from the same tree. The mechanism is
unchanged and is documented in that file; the reasoning behind its two
load-bearing decisions was paid for in the Arland project, so it is repeated
there rather than re-derived:

- The redirect is **armed** in `DllMain` and **runs** at the host executable's
  entry point. Starting a child from process attach produced a process Steam
  knew nothing about, which cost the overlay and Steam Input.
- The stock launcher process **stays alive** behind whatever replaced it,
  because it is the process Steam launched and is counting.

What differs from Arland is only the naming: the host must be one of the three
`Atelier_<Game>Launcher.exe` files, the ini is `dusk-fix.ini`, the stand-down
variable is `DUSK_NO_REDIRECT`, and the configurator it looks for is
`dusk-fix-launcher.exe`. The game build to start on a straight launch is chosen
from `[Lang] Language` in `Setting.ini`, using the mapping read out of the stock
launcher below rather than assumed.

The entry point is taken from the PE headers rather than a hardcoded RVA, and
the original five bytes are kept so a failed start restores them and runs the
stock launcher as though the mod were not installed. With no
`dusk-fix-launcher.exe` installed the redirect is never armed at all, so a
partial install is inert rather than broken.

## The launcher window

`dusk-fix-launcher.exe` (`src/launcher/launcher_gui.cpp`) is the 64-bit settings
window the proxy opens. It is **structured to match the Arland project's
`src/config_gui/main.cpp` deliberately and closely**, so that someone who has
used one does not have to learn the other: the same tab strip with the game name
right-aligned on it, the same cursor-driven `Layout` with measured note heights,
the same bottom button row with Play on the left and the skip-launcher checkbox
beside it, the same About page, the same mnemonics.

It carries three of Arland's four pages, named identically: **Display**, **Game**
and **About**. Image Quality is absent because this mod has no image-quality
options at all -- no MSAA, supersampling, anisotropic filtering, shadow-map
scaling or SMAA -- and an empty tab is worse than a missing one. Arland's
conditional Debug tab is absent for the same reason: there are no developer
views to reach, and the diagnostics that do exist are environment switches on
purpose.

The `Layout` helper is carried over rather than reinvented, including the part
that matters most: **every note's height is measured at the width it will be
drawn at**. A static silently drops any line past its height, so a note given a
fixed two lines turns a third line into a sentence ending mid-word, with nothing
in the build, the log or the code to say so. That shipped twice in the Arland
project.

It edits two files, and the split is the interesting part:

| File | Keys | Whose |
|---|---|---|
| `Setting.ini` | `[Graphics] ScreenWidth`/`ScreenHeight`/`Outline`, `[Window] FullScreen`, `[Lang] Language` | the game's own, read whether or not the mod is installed |
| `dusk-fix.ini` | `[Launcher] SkipLauncher`/`AutoResolution` | the mod's |

### Auto resolution, and why it resolves in a different place than Arland's

The resolution list leads with an **Auto** entry carried as a 0x0 sentinel,
exactly as the Arland launcher carries it, and labelled with what it currently
resolves to (`Auto  (1920 x 1080)`) so the answer sits in the list being chosen
from.

The two projects have to *resolve* it in different places, and that is a
consequence of the resolution decision above rather than a style difference.
Arland leaves its own ini keys blank and its DLL decides what blank means when
the device is created, so Auto there keeps following the display even if it
changes between launches. This mod writes a literal number into the game's own
`Setting.ini`, because the game reads that field itself and no mod-side
resolution override exists to duplicate it -- and "Auto" is not something the
game's integer parse can be handed. Its reader replaces only a *negative* value
with a default, so a zero written there would be passed straight through.

So Auto is resolved here, at save time, and the choice is remembered in
`[Launcher] AutoResolution` so that reopening the window shows Auto rather than
whatever number it last resolved to.

Two corrections to what this section used to claim.

**Auto's default was wrong, and that was a real divergence from Arland.**
`loadFromIni` passed `false` as the default for `AutoResolution`, and that key
is never seeded at file creation (only `SkipLauncher` is), so any install whose
launcher had not yet saved selected the literal from `Setting.ini` instead of
Auto -- the game's shipped 1280x720 on a fresh one. Arland selects Auto in
exactly that case (`int baseSel = 0; // Auto by default`), and `resetToDefaults`
here already argued that a fresh install must not inherit 720p. The default is
now `!(width && height)`: Auto when the game's file carries no resolution of its
own, the file's value when it does. A flat `true` would have been the closer
mirror of Arland but would break this window's other rule, that opening it never
silently replaces a resolution the user already chose -- a deliberate 4K on a
1080p panel has to survive being looked at.

**Arland's mechanism does not port, and blank `[Rendering] DisplayWidth` /
`DisplayHeight` keys in `dusk-fix.ini` do nothing.** Arland can present at a
size the game never chose because it carries a render-to-display fit pass
(`supersample.cpp`) that bridges the two, and `sync_fix.cpp` records the
original swap-chain size when it overrides the chain. Dusk has no such pass. A
swap chain overridden to the desktop resolution while the engine sizes its own
targets from `Setting.ini` puts the scene in a corner of an oversized
backbuffer, and on Ayesha it would also stop `highres.cpp` adopting a main
render size at all, since nothing would match the chain any more. Porting the
keys means porting the fit pass first.

What was genuinely lost against Arland is therefore narrower than "needs the
DLL": it is only the launches where the launcher window never opens, since
`Start game` saves first and re-resolves Auto every time it is used. That gap is
now closed in the 32-bit proxy instead (`applyAutoResolution` in
`launcher_proxy.cpp`), which resolves the display and writes the game's own
`Setting.ini` before starting either target. It covers the `SkipLauncher` path
and the stock-launcher path, needs no rendering machinery, and leaves the
resolution real rather than imposed on top of the game. It cannot cover starting
the game executable directly, which is what the workspace's own `run-*.sh`
scripts do -- there, whatever is in `Setting.ini` is what runs. `Reset to defaults` selects Auto, so a fresh
install never inherits the game's own 1280x720.

One Arland behaviour is deliberately **not** carried over: dropping modes larger
than the display maximum. Ayesha windowed will create a swap chain bigger than
the screen, and that is how the 1440p render-target census was measured on a
1080p panel; filtering by the display maximum would have removed the one entry
that made it possible. The cost is that a fullscreen selection above the panel's
size is offered and will not do anything useful.

Resolution is written to the game's file and nowhere else, because the game
already owns that setting and accepts any value in it. The list offered is this
project's own and is **not** filtered through Windows' reported display modes,
which is the point: that filtering is what hides a usable mode on a high-DPI
handheld or in docked use. The current desktop mode is appended if it is not
already listed, as is whatever the file already holds, so opening the window can
never silently change a resolution it did not offer.

Three behaviours are carried over from the Arland launcher because each was paid
for there:

- **It configures the folder it was run from, not always the one it lives in.**
  Wine resolves a symlinked executable before reporting it, so a launcher
  symlinked into a game folder reports the link's target as its own location.
  The working directory is the fallback, and it is what Explorer, the test
  scripts and the msimg32 redirect all set correctly.
- **Start game saves first.** Starting with the settings only on screen is the
  one outcome nobody wants from that button, and it matters most for the game's
  own file, which it reads either way.
- **The two stock-tool buttons set `DUSK_NO_REDIRECT`.** The proxy sent the
  stock launcher here in the first place; without it, that button would only
  ever reopen this window. It is cleared immediately so it cannot reach the game
  from a later press of Play with mod.

The Display page's third button, "Play without the mod", needed one addition on
the DLL side: `DUSK_DISABLE`, checked in `initializeEngineFixes()`. When it is
set, Direct3D is still forwarded but no engine module initializes and nothing is
hooked, so the game as it shipped can be compared against without moving files
out of the folder and having to remember to move them back. It is the Arland
`ARLAND_DISABLE` mechanism under this project's prefix.

The window offers the game's own settings and `SkipLauncher`, and none of the
mod's fixes. Every one of them is on by default and none is a preference: the
font-atlas cache and the field-jitter halves have nothing to trade off, and the
high-resolution correction's only trade-off is the resolution itself, which the
player has already chosen in this same window. Asking again with a checkbox
would be the same question twice.

A consequence worth noting: the window no longer varies by game at all. The
engine flag that used to grey the Ayesha-only fix controls on Escha & Logy and
Shallie has been removed along with the controls, so all three games get an
identical window and the per-game difference lives entirely in the DLL's
capability matrix.

The tabs are **Display, Image Quality, Game, About**, which is the Arland
launcher's arrangement (it adds a Debug page the Dusk mod has no equivalent
for). Image Quality exists for the same reason it does there: it holds the
settings that cost frame rate, so that cost sits in one place instead of being
scattered among settings that have none. It carries SMAA, the MSAA sample count
and the supersampling multiplier, and nothing else in this window has a
performance cost worth grouping with them.

### Matched against the Arland launcher, control by control

| Arland control | Dusk |
|---|---|
| Resolution, with Auto | present |
| Window mode | partial: Windowed/Fullscreen. Arland also offers **Borderless**, which is a mod feature there (`window_mode.cpp`) and does not exist here |
| Settings editor / Original launcher / Play without the mod | present |
| Supersampling, with the computed size per entry and the 8K ceiling | present |
| Anti-aliasing (MSAA) | present |
| Edge smoothing (SMAA) | present |
| Language | present |
| Character outlines | present |
| Preset | absent: Arland's spans five settings, Dusk has three, so the ladder would mostly restate the individual controls |
| Texture sharpness (anisotropic) | **no backing feature.** This is the queued `CreateSamplerState` work in DUSK.md; the control follows the fix, not the other way round |
| Shadow detail | **no backing feature** (Arland's `ShadowMultiplier` has no Dusk equivalent) |
| UI font | **no backing feature** (high-resolution font rendering is a queued enhancement) |
| Battle cut-in shadows / dimming | Arland-specific, and Rorona/Meruru-specific at that |
| Verbose logging | **no backing feature**: this project has no verbose-logging concept |
| Debug view | **no backing feature**: Dusk's only debug view is `DUSK_SMAA_DEBUG`, and diagnostics here are environment-only by policy |

The supersampling list is the one that took real work to match rather than
copy. Each entry carries the size it produces (`1.5x  (3840 x 2160)`), so what a
multiplier actually costs is visible in the list being chosen from; entries that
would exceed the 8K ceiling are omitted rather than offered and then quietly
reduced by the DLL; and because the list is filtered, its positions are not the
table's positions, so the selected entry is read back through its item data. It
is rebuilt whenever the resolution changes, since that is what it multiplies.

The multiplier is stored as a **percentage** (`150`), not a decimal. "1.5" in an
ini is a locale trap: under a locale whose decimal separator is a comma it
parses as 1, silently turning supersampling off. An integer has no such reading.

An earlier single-page version of this window was opened once and came up. The
tabbed structure above has not been run: it compiles and is deployed to the
Ayesha test folder, but no run has confirmed the tab layout, the measured note
heights at either scale, the save round-trip into both files, Auto resolving to
the desktop mode, the redirect chain, the two stock-tool buttons or "Play
without the mod".

## Configuration: dusk-fix.ini

The project now has an ini layer, `src/core/config.{h,cpp}`, because the
launcher needs somewhere to persist what it configures. It follows the Arland
split exactly: **`dusk-fix.ini` is the user-facing surface, and an environment
switch on its own is not one.** A diagnostic with an ini key would eventually be
turned on by someone following a forum post, and these diagnostics are slow on
purpose.

So the capability matrix's `Descriptor` carries an optional section and key
alongside its environment variable, and only the features a user is meant to
choose between have one:

| Key | Feature |
|---|---|
| `[Launcher] SkipLauncher` | start the game directly, skipping both front-ends |
| `[Launcher] AutoResolution` | remembers that Auto was picked, so the launcher keeps following the desktop |

Having a key is therefore not the same as being on by default, and the shipping
fixes are the case that separates them. The font-atlas read cache and both
field-jitter halves ship on and have no key at all. Nothing about them is a
judgement a player can make: the cache is strictly faster with no visual
difference, and the field halves are one coupled fix (the stabilizer refuses to
run without the rescale), so the only combinations a key could express are the
fix and a broken half of it. `DUSK_ATLAS_CACHE=0` and `DUSK_FIELD_ENGINE_FIX=0`
cover the A/B, which is the only reason to stand either down.

The high-resolution correction looked like the counter-example that would keep
this from being a blanket rule, and it is not one. Rendering at 4K instead of
1080p does cost real performance, so there is a decision -- but the player makes
it when they choose a resolution, and the fix only makes that choice honest. A
key asking whether the chosen resolution should be the one rendered is the same
preference asked twice. An option a user cannot usefully answer is not an
option, and putting it in the file only invites turning it off.

Precedence is environment, then ini, then the matrix default, and
`Support::Unsupported` remains a hard off that neither can turn on. Only
`SkipLauncher` is seeded when the file is created; the feature keys are seeded
lazily on first read, which is what keeps each one written with the default for
the game it is actually running in, and keeps a game that supports none of them
from growing keys it would ignore. Escha & Logy and Shallie therefore get a
`dusk-fix.ini` containing `SkipLauncher` and nothing else, which is an accurate
description of their current state.

`logConfiguration()` writes the resolved values into the log at startup, so a
report says what the run was configured with rather than what the reporter
believes it was.

## Diagnostics

Each session log begins with the version compiled from the repository's `VERSION`
file, the resolved title and engine, and the D3D11 forwarding route. Each engine
module then reports what it recognized and what it installed, so a log makes
clear whether a fix declined on a fingerprint, on a prologue check, or was simply
not asked for.

`DUSK_ATLAS_STATS=1` is the font-atlas diagnostic that justified the cache. It
installs the four verified hooks in counting mode — nothing is cached, nothing is
suppressed, every hook forwards straight to the original — and reports per drain
and per frame: `drainMicros` or `frameMicros`; `lockMicros`, the wall time inside
the atlas-lock hook measured across the whole body so cache-served locks are
charged their real cost rather than vanishing; `renderMicros`, the outermost
text-renderer call only; and `aboveAtlasMicros`, their difference — the CPU-side
glyph and layout work *above* the atlas, which is precisely what an atlas cache
cannot touch. It also reports a dimension histogram, which exists to validate the
`+0x40/+0x42` offsets rather than assume them: garbage there invalidates every
count above it. With the cache on it adds a `churn:` line reporting cache misses
by access mode, unmatched unlocks, and snapshots dropped.

`DUSK_ATLAS_TRACE=1`, which requires `DUSK_ATLAS_STATS=1`, records the raw
out-of-drain lock/unlock sequence of **one** steady-state frame and prints it as
a token stream. `[` and `]` are renderText enter and exit; a lock is
`<tex><w|r><outcome>` with `+` hit, `*` real and snapshotted, `-` real without a
snapshot, `n` non-candidate; an unlock is `<tex>u<outcome>` with `s`
synthetic-suppressed, `m` matched, `X` snapshot dropped, `.` unmatched with
nothing to drop. The trace picks its own frame rather than asking for one: past
120 frames of warm-up, the first frame that rendered text with four or fewer
render calls and did not overflow the 2048-event ring. It dumps once and stops.
One frame is about 266 events, so the output is a dozen lines.

`DUSK_ATLAS_VERIFY=1` is the cache's correctness check, and requires the cache to
be on since it checks what the cache serves. Two independent things are asserted.

First, a snapshot that has **not yet absorbed one of the mod's own served
writes** must be byte-identical to the real texture. Once the cache serves a
write from a snapshot the game rasterizes into that buffer instead of the real
atlas, so divergence after that point is expected and the comparison stops until
the frame boundary replaces the snapshot. Any divergence reported before it is a
write the cache did not know about, and the log line carries the texture, the
first differing byte and its (x, y) in the atlas, which is what identifies the
glyph.

Second, a write-mode lock only reaches the real middleware when the cache did not
serve it, and the cache serves every cacheable write for which a snapshot exists.
So a real write lock on a 512×512 atlas that already has a live snapshot is
necessarily a write from outside the text renderer, and is reported as such. This
covers the window the comparison cannot: the write mapping is object-held rather
than scoped, so time can pass between such a write and the unlock that would
invalidate the snapshot, and a read served in that window gets stale bytes.

The mode costs a real atlas lock and a ~1 MB comparison per verified read, so it
is a diagnostic and never a shipping configuration.

It reports a running tally every few hundred frames, **independently of
`DUSK_ATLAS_STATS`**. That independence is the point rather than a convenience: a
correctness check whose only output is "no findings" cannot be distinguished from
one that never ran, so the tally always carries the number of checks that
produced it. A clean session is one where `checks` is large and `mismatches` and
`foreignWrites` are both zero; a session where `checks` stays at zero has proved
nothing and is itself a finding.

`DUSK_TARGET_CENSUS=1` is the render-target census, and is the one diagnostic
that is not Ayesha-only: it reads nothing but the D3D11 resources the game
creates, so it needs no mapped address and no engine knowledge, and it lives in
`src/core` rather than in either engine module. It hooks
`ID3D11Device::CreateTexture2D` and logs one line per distinct (dimensions,
format, bind flags, sample count, call site) tuple that is bound as a render
target or a depth-stencil, classifying each against the swap chain the game
actually got: `rel=matchesSwapChain`, `rel=1920x1080`, or `rel=other`. The
swap-chain line itself is logged in every run, census or not, since one line
naming the present resolution is worth having in every report.

Each line also carries `action=`, which says what the high-resolution fix did
with that creation: `adoptedAsMain`, `resizedFull`, `resizedBlur` or
`passthrough`. That is why the two share a file -- one log answers both "what
does this game create" and "what did the fix do about it".

It is keyed on the target's *shape* rather than on the resource pointer,
because the engine recreates these objects and the question is which shapes
appear, not how many objects carried them. It reports the count of creations
with every periodic summary for the same reason `DUSK_ATLAS_VERIFY` reports its
check count: a census whose only output is "nothing found" cannot be
distinguished from one that never installed. It is opt-in on all three games and
initializes MinHook itself, since on Escha & Logy and Shallie it is the only
thing in the tree that hooks anything.

`DUSK_FIELD_TRACE=1` wraps the field controller's per-frame update and, on each
ground-contact change, dumps a short ring of frames either side of the event. It
only writes inside those windows, so a character resting quietly produces no
output at all.

### The field-jitter fix

The Ayesha field-jitter implementation is a port of Arland's threshold rescale
and resting stabilizer. Its anchors resolve in both Ayesha builds. An early
in-game test reported that it did not fix the problem, and it was held OptIn on
that basis; a later session with **both** switches on confirmed that it does, and
both are now OnByDefault.

The earlier negative is worth keeping rather than deleting, because the most
likely reading of it is a configuration result rather than a code one: the
stabilizer refuses to run without the rescale, so a test that enabled only one of
them, or that relied on an ini key while an environment variable pinned it off,
would report exactly that failure. `04-run-ayesha.sh` did unconditionally export
`DUSK_HIGHRES=0` for months, and `featureEnabled()` reads the environment before
the ini, so the hazard is demonstrably real in this workspace. Neither switch
should be judged again without a log line confirming what was actually in force.

What is measured and what is inferred, stated plainly: the defect and its shape
are measured (below), and the fix is confirmed by playthrough. The mechanism
described here, gravity integrating against the surface while sub-threshold moves
are discarded, is the reading the code comments and the measured sawtooth agree
on, not something a trace has confirmed instruction by instruction.

#### A second configuration trap, in the same shape as the first

Promoting the two halves in the capability matrix did not turn them on. The
matrix said `OnByDefault`, `logConfiguration()` printed `FieldEngineFix = on`
and `FieldStabilizer = on`, and the module still logged
`FIXES field_physics=off` on the next run.

The cause was `engineFixEnabled()` and `stabilizerEnabled()` in
`field_physics.cpp`, which each read their environment variable directly with
`std::getenv` and returned false when it was absent. They predated the matrix
carrying these features and were never rewired, so the promotion had no path to
reach them: `phyre.cpp` asked `featureEnabled()` and installed the module, and
the module then asked itself and declined. Both now call `featureEnabled()`,
which is the only thing that knows about the matrix, the `Unsupported` hard-off
and the environment override at once.

This is the same failure as the `DUSK_HIGHRES=0` pin in `04-run-ayesha.sh`: a
second, partial answer to "is this feature on" that disagrees with the real one.
It is also the second time the log's `Config:` block is what made it findable,
which is the argument for keeping every shipping fix listed there even though
none of them has an ini key any more. A sweep for `getenv` outside
`featureEnabled()` now finds only `DUSK_FIELD_TRACE` (a diagnostic with no
matrix entry) and `DUSK_DISABLE` (the global kill switch), which are both
legitimately direct.

#### The controller offsets, confirmed on both builds

The offsets were carried over from Arland and were for a long time the one part
of this the evidence did not cover. They have now been checked statically
against both Ayesha builds, scoped to the controller update
(EN `0x739fa0`, ML `0x75c4a0`), and every one holds. The two the stabilizer
actually *writes* are the two with the strongest evidence:

| Offset | Use | Evidence |
|---|---|---|
| `+0x54` | vertical velocity (**written**) | accumulated (`addss xmm1, [rbx+0x54]` then stored back) and clamped with `mov dword [rbx+0x54], 0xc1a00000`, i.e. `-20.0f` terminal velocity |
| `+0xb8` | air timer (**written**) | reset to zero, then accumulated by frame time and stored back |
| `+0x50` | velocity vector | written as a whole with `movdqa`/`movups`, so `+0x54` is its Y as assumed |
| `+0x60` / `+0x70` | live position / entry copy | the resolver reverts a discarded move with `movaps xmm0,[rsi+0x70]` then `movdqa [rsi+0x60],xmm0`, and differences them componentwise at `0x738955` |
| `+0xb0` | foot height | read against position in both the update and the resolver |

`+0x38` needed a correction rather than a confirmation. It is not a flags word:
`+0x38` and `+0x39` are two adjacent **byte** flags, and ground contact is the
one at `+0x39` (set to 1 in the same breath as the air-timer reset at
`0x73a0c1`, cleared alongside the velocity write at `0x73a136`). Reading a
`uint32` at `+0x38` and masking `0x100` therefore lands on byte `+0x39` bit 0
and is correct in effect, which is why it has always worked, but the constant is
doing something less direct than `kGroundedOffset`/`kGroundedBit` suggest. It is
only sound while that byte holds 0 or 1, which is what every write site in both
builds does.

Both builds are structurally identical here: same offsets, same instruction
shapes, at the same relative positions within the update (`0x73a0c1` against
`0x75c5c1`, and so on).

| Anchor | Ayesha EN | Ayesha multilingual | Static evidence |
|---|---:|---:|---|
| controller `Update` | `0x739fa0` | `0x75c4a0` | MATCH from Meruru; prologue byte-identical |
| collision resolver | `0x738670` | `0x75ab70` | MATCH from Meruru |
| move-threshold data | `0x1627f20` | `0x17bf8e0` | unique writable-data match for `0x3c0b4396` (`0.0085f`), with one verified reader |

Further work must begin with the trace and fresh Ayesha-specific analysis rather
than another blind port. Confirm the reported fields and offsets against the
live controller, then trace the controller update and collision path to find the
actual frame-rate-dependent state before allowing any new write.

#### A measured baseline, and what its shape rules out

A 1920x1080 60 fps capture of a 144 Hz session (`FieldEngineFix = off`,
`FieldStabilizer = off`, so this is vanilla behaviour rather than a failing fix)
was measured by collapsing each frame to a 1-D intensity profile and recovering
sub-pixel displacement by SSD with a parabolic refinement. Ayesha is standing on
the atelier's interior steps. The clip separates cleanly into four phases:

| Frames | Background vertical | Character vertical | Character horizontal | Reading |
|---|---:|---:|---:|---|
| 0-59 | 0.00 | 0.01 | 0.00 | pixel-identical, nothing moving |
| 60-99 | 12.89 | 26.89 | 7.26 | she moves; camera follows |
| 100-179 | 2.4-3.5 | 11.6-17.7 | 0.5-6.0 | **the jitter** |
| 180-237 | 0.00 | 0.01 | 0.00 | pixel-identical again |

(peak-to-peak pixels within each 20-frame block)

Three properties of the jitter window matter more than its amplitude:

- **It is vertical only.** Horizontal character displacement falls to ~0.5 px
  after frame 120 while vertical stays at 12-18 px for a further second. She is
  horizontally at rest and still oscillating, which is the resting case rather
  than a movement-integration case.
- **It does not decay.** Amplitude is as large at frame 170 as at frame 105.
  This is a sustained relaxation oscillation, not an overshoot settling out. The
  per-cycle shape is a slow ramp of 1-3 px over five or six captured frames, then
  a single frame displaced 13-18 px, then a snap back to the base position.
- **It stops absolutely.** At frame ~179 the whole frame becomes pixel-identical
  and stays so for a second. Nothing damps; it simply ceases.

That shape is the **sawtooth this file already describes** in the comment on
`applyRestingStabilizer`: "gravity keeps integrating against a surface, so a
frame still breaks through every few frames". Under that reading the ramp is
gravity accumulating vertical velocity while the resolver discards each
sub-threshold move, and the single displaced frame is the accumulated move
finally clearing the 0.0085 threshold and being applied whole. The measurement
is independent corroboration of that mechanism, arrived at from screen space
without reference to it, and it is the first time the sawtooth has been
quantified rather than described.

It is emphatically **not** evidence that the ported implementation fails, because
the capture was made with both switches off. The resting stabilizer exists for
exactly this case and was not running. What the baseline gives is the thing to
measure any candidate fix against: the same spot, same capture settings, with the
switches on, should flatten the 12-18 px vertical excursion toward the ~0 px the
settled phases already show.

The static side of the resolver has been mapped to support that work; see the
next subsection.

Three runs are worth capturing together, none of which has been done:
`DUSK_FIELD_TRACE=1` over this spot (which needs neither switch on and reports
windows around contact changes), the same spot with both switches on, and the
same spot at 60 Hz rather than 144 Hz.

Capture caveat: the recording samples a 144 Hz session at 60 fps, so every
frequency here belongs to the capture and not to the game. The amplitudes and
the phase structure survive the aliasing; a period in Hz does not, and none is
claimed.

#### Static map of the collision resolver

Ayesha EN `0x738670`, a single `.pdata` function of `0xb9b` bytes. Every
constant it loads, decoded:

| RVA | Value | Reading |
|---|---:|---|
| `0x1627f20` | `0.0085` | the documented move threshold, read at `0x738c69` |
| `0x10f8bc8` | `1.0471976` | pi/3, i.e. a 60 degree limit, tested at `0x738c55` |
| `0x10f8bd4` | `-0.8` | dot-product threshold, `0x738935` |
| `0x9b4dbc` / `0x10f8bd0` | `+/-1.1920929e-05` | symmetric dead band |
| `0xd2c904` / `0xd2c910` | `+/-0.0011920929` | second symmetric dead band |
| `0x9896f8` | `0.01` | |
| `0x98c308` | `4.0` | multiplier on the `0.0011920929` band, `0x738947` |
| `0xbcaf9c` | `-5.0` | |
| `0x989138` | `1.0` | |

Two structures in it matter for this investigation.

**The 60 degree test at `0x738c48`.** A value built from a normalize-and-dot
chain is compared against pi/3, and on the greater-than side the function stores
`r14d` to `[rsi+0x54]`. `+0x54` is the controller's vertical velocity (confirmed
below), so this is a slope limit that kills upward or downward velocity on a
surface too steep to stand on, not a flag write.

**The candidate-vector selection at `0x738926`-`0x738986`.** Guarded by three
tests (a magnitude against the `0.0011920929` band, the `-0.8` dot threshold, and
a magnitude against `4.0x` that band), the function picks between two movement
vectors before taking its length:

- the delta `[rsi+0x70..0x78] - [rsi+0x60..0x68]`, the difference between the
  controller's live position (`+0x60`) and the copy taken on entry (`+0x70`);
- otherwise a vector already in registers, the raw requested motion.

A `-0.8` dot threshold selects on *direction reversal* of roughly 143 degrees or
more, which is the shape of an anti-oscillation guard the engine already carries.
Whether it is failing to engage in the resting case, or engaging and being
defeated downstream by the threshold, is the question a trace should settle
before any new write is designed. Nothing here has been confirmed against the
live controller, and none of it should be treated as more than a map until it
has been.

## The "Loadning system data." typo

> **TL;DR**: Escha & Logy and Shallie misspell "Loading" on the first screen of the game. The word is a plain string literal in each executable's `.rdata`, so the mod corrects the 22 bytes in the loaded image at startup. First fix shipped for the KTGL module. Nothing on disk is touched.

### The defect

Both KTGL games print a status line while they read the system save on the very
first screen, and the English string reads `Loadning system data.` It is one of
the first things a player sees.

It is the localization's error, not the engine's, and the evidence for that is
the neighbourhood the string sits in. `Saving system data.`, `Saving System data.`
and `Deleting…` immediately around it are all correct, and so are the Japanese,
Simplified Chinese and Traditional Chinese variants of the same line that sit
beside it in the same block.

### Where it lives, and why that settles whether it is fixable

A narrow (single-byte) string literal in `.rdata`, reached directly by `lea`
instructions in the save/load status code. Not a wide string, not an entry in a
pointer table, and **not** in any game data file — a recursive scan of
`Data/`, `DLC/`, `Script/` and the `Event*` trees for the misspelling finds
nothing. That is what makes this fixable at all: had it lived in a data file the
only route would have been intercepting the file at load time, which is a far
larger job than the defect is worth.

Exactly one occurrence per executable, in all four builds:

| Executable | File offset | RVA | `lea` references |
|---|---:|---:|---:|
| `Atelier_Escha_and_Logy_EN.exe` | `0x7cd4d8` | `0x7ceed8` | 3 |
| `Atelier_Escha_and_Logy.exe` (multilingual) | `0x85c378` | `0x85e978` | 3 |
| `Atelier_Shallie_EN.exe` | `0x76d720` | `0x76f320` | 2 |
| `Atelier_Shallie.exe` (multilingual) | `0x7c67c0` | `0x7c89c0` | 2 |

The multilingual builds carry the English string too, so both builds of both
games need the correction. Ayesha has neither the string nor the defect — it is a
different engine and a different localization pass — which is why its row in the
capability matrix is `Unsupported` as a statement about the binary rather than
for want of evidence.

The RVAs were derived from the file offsets through the PE section table
(`.rdata`'s `rawptr`→`va` delta) and confirmed with
`atelier-re-tools/tools/atre.py bytes`; the reference counts come from
`atre.py xref-data`. `typo_scan.py` was not the instrument here — it is shaped
for the Arland layout (`*_Release_en.exe` plus `Res/x64/WinXls_EN`) and finds
neither in a Dusk install.

### The correction

`src/engines/ktgl/loading_text_fix.cpp` overwrites the literal in place at
startup:

```
Loadning system data.\0     22 bytes, shipped
Loading system data.\0\0    22 bytes, written
```

The correct spelling is one character shorter, so the replacement fits inside the
region the compiler already reserved; nothing after it moves, and the second NUL
clears the byte the shortened string vacated. Only the four `.rdata` addresses
above are ever written, and only after the bytes there are compared against the
full shipped literal — an RVA that has drifted, or a build already patched by
something else, fails that check and the fix declines with a logged reason rather
than overwriting 22 bytes of unrelated read-only data.

`.rdata` is mapped read-only, so the page is switched to `PAGE_READWRITE` for the
store and its original protection restored immediately afterwards. This is the
one place in the project where the `VirtualProtect` call is load-bearing rather
than defensive: `field_physics.cpp` protects a page whose section flags already
say writable, this one would fault without it.

Cost is one 22-byte `memcpy` at initialization. There is no hook, no detour, no
trampoline, and no per-frame work — the game reads the corrected bytes because
they are the bytes at the address its own code already points at. It is on by
default (`DUSK_LOADING_TEXT=0` stands it down for a comparison) and has no ini
key, on the standing rule that a correction is not a setting.

### Gating

`initializeKtglFixes` now requires a **verified** fingerprint before installing
anything: the executable name must match a row in `kGames` and the loaded
`.text` VirtualSize must equal that row's. A name match with a mismatched size is
logged as `MISMATCH` and installs nothing, because every RVA in that table was
read out of one specific compile. The per-build RVA moved into `atfix::KtglGame`
in `ktgl.h`, mirroring `atfix::PhyreGame`: the module entry point recognizes the
executable once and hands each fix the base plus the verified descriptor.

### Not yet confirmed in game

The correction is verified statically — the bytes, the addresses and the
references are all read out of the four shipped executables — but no run has been
made to see the corrected line on screen. That is a one-launch check on Escha &
Logy: start the game and read the status line on the first screen. The log line
to look for is `FIXES loading_text=active rva=0x...`.

## Runtime memory manipulation

> **TL;DR**: The mod does not edit or replace the games' executable files. Its proxy DLL is loaded alongside the game, makes narrowly verified changes inside that running process, and forwards everything else to the real Windows library. Those changes disappear when the process exits.

The 64-bit `d3d11.dll` is a proxy for the system D3D11 library. It exports the
device-creation functions the game expects and forwards them to
`d3d11_proxy.dll` when one is deliberately installed for chain-loading, or to the
real `d3d11.dll` from the Windows system directory otherwise. This lets the mod
observe device creation and install its hooks without replacing the graphics
implementation.

The 32-bit `msimg32.dll` is the second proxy, loaded by the games' own
front-ends rather than by the games. It forwards `AlphaBlend` and
`TransparentBlt` to the system library and, in the three launcher processes
only, rewrites five bytes at the executable's entry point so the launch can be
redirected. The original bytes are kept and put back if the redirect cannot
complete. See "The launcher proxy" above.

Executable changes are almost all detours. After verifying the target function,
MinHook places a jump at its entry point and preserves the displaced instructions
in a trampoline, so the mod can do work before or after normal engine behaviour
rather than replacing the routine. MinHook is used rather than this project's own
fixed-size absolute-jump installer because the hooked atlas unlock is a 14-byte
stub, too small to patch without clobbering what follows.

The one exception is data rather than code: Escha & Logy's and Shallie's
`Loadning`→`Loading` correction rewrites 22 bytes of a `.rdata` string literal at
startup, after comparing them against the full shipped literal, and restores the
page's original read-only protection immediately. It installs no hook and runs no
code afterwards.

Where the mod reads or writes engine memory it proves the address first: shared
guarded-read helpers reject unavailable memory before any reverse-engineered
pointer chain is followed, and a range is checked committed and writable in one
query before any offset within it is touched.

All code patches, trampolines, cached pointers and snapshots exist only in the
current process. Ending the game discards them, and removing the proxy DLL
restores an entirely unmodified executable on the next launch.

## Hook boundaries

Hooks install only after the process is recognized by executable name and `.text`
VirtualSize, read from the loaded headers rather than the file so a build that is
packed on disk still matches once its stub has decrypted the section. Every
target is then byte-checked against its complete expected prologue before MinHook
is invoked. Nothing is written to any Koei Tecmo executable.

Ayesha ships two executables — an English build and a multilingual one, separate
compiles with distinct RVAs. Both are fingerprinted:

| Executable | SHA-256 | `.text` |
|---|---|---:|
| `Atelier_Ayesha_EN.exe` | `b10a07e494abfb5f5711aeb9151b894662b105a3478ac245bf38604d5a6e360d` | `0x984df4` |
| `Atelier_Ayesha.exe` (multilingual) | `95437b9fa746af9c0947a81afd6a671877dc30d22742dd2de197e054202cef22` | `0x9a9604` |

The atlas cache verifies four independent entry points per executable:

| Anchor | EN | Multilingual | EN→ML verdict |
|---|---:|---:|---|
| Queue drain | `0x078320` | `0x07a8d0` | MATCH, 29 votes, prologue identical |
| Text renderer | `0x74bd90` | `0x76e290` | MATCH, 37 votes, runner-up 0 |
| Atlas lock | `0x581420` | `0x5a3920` | WEAK vote; corroborated, 11 callers via thunk `0x3abc` in both |
| Atlas unlock (stub, hooked) | `0x581460` | `0x5a3960` | stub at lock+`0x40` in both; per-build array |
| Atlas unlock (impl) | `0x581470` | `0x5a3970` | MATCH, 31 votes |

Prologues, 16 bytes. The queue drain, text renderer, lock and unlock
implementation are build-independent; the unlock stub is not, because its window
carries the `jmp rel32` displacement.

```
queueDrain      48 8b c4 55 41 54 41 55 41 56 41 57 48 8d 68 98
renderText      48 8b c4 48 89 50 10 53 48 81 ec 90 00 00 00 48
atlasLock       48 83 ec 38 44 89 4c 24 20 45 8b c8 45 33 c0 e8
atlasUnlockImpl 48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57
atlasUnlockStub 44 8b c2 33 d2 e9 01 ff a7 ff cc cc cc cc cc cc   (EN)
atlasUnlockStub 44 8b c2 33 d2 e9 01 da a5 ff cc cc cc cc cc cc   (multilingual)
```

The lock's window ends on the `e8` call opcode and deliberately excludes the
displacement, which is why it is portable across builds. Each row was derived by
matching the corresponding Arland entry point into the Ayesha English build, then
matching English to multilingual; the lock's and unlock's callers were enumerated
through their jump thunks. The Arland source addresses used were Meruru's queue
drain, text renderer and atlas lock, and Rorona's atlas unlock implementation.

Within `renderText` exactly two paths reach the atlas lock, and both carry a
hardcoded access mode. The read is `renderText+0x290` (`xor r9d, r9d`, mode 0),
locking the atlas and blitting the glyph out over a loop that contains no call
instruction before the matching unlock at `renderText+0x30c`. The write is
reached at `renderText+0x149` through `0x5aa770` into `0x5a9ae0`
(`lea r9d, [rax+3]`, mode 3, `WRITE_DISCARD`) and released through `0x5abc60`
into `0x5a9fb0`. The write mapping is held as engine-object state — `0x5a9ae0`
stores the mapped pointer at `obj+0xa0` and short-circuits when it is already
non-null — rather than as a scoped bracket like the read.

Escha & Logy and Shallie have no menu hooks and no detours of any kind. They now
carry one fix — the `Loadning`→`Loading` correction, which patches a string
literal rather than hooking anything (see "The 'Loadning system data.' typo") —
and the capability matrix hard-offs every other feature for them so no
configuration can turn one on. Their four executables are fingerprinted the same
way, and that fingerprint must now *verify*, not merely match by name, before
anything installs:

| Executable | SHA-256 | `.text` |
|---|---|---:|
| `Atelier_Escha_and_Logy_EN.exe` | `97485fba0ede484de8279b24a2adce5ff1c9575a88928821fd8a88ecd1f5e192` | `0x715e8c` |
| `Atelier_Escha_and_Logy.exe` (multilingual) | `cd0b69c8c84ced23dfa76fff7fb88b3006ac11ad79a6b6384af7f29a0bf22b18` | `0x73739c` |
| `Atelier_Shallie_EN.exe` | `c01bb4a22d33705b1e3108e9142003424f03bbf6347650c1184b00c02715055a` | `0x6bca4c` |
| `Atelier_Shallie.exe` (multilingual) | `ea453ca0eee4a0391a3be359311f2878e89c98a06ed3c56cb92ff08d60e8e35d` | `0x6ff53c` |

Per-game availability and defaults are centralized in a capability matrix
(`src/core/game.cpp`): the running title is detected from the executable name
independently of any hook, and each feature resolves through the matrix —
unsupported titles are hard-off regardless of configuration — before consulting
its environment override, then its `dusk-fix.ini` key if it has one. The matrix
is the source of truth mirrored by the feature table in the README. See
"Configuration: dusk-fix.ini" above for which features have an ini key and why
the diagnostics deliberately do not.
