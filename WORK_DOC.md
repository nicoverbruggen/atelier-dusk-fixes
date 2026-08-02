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
suppressed. **Any unmatched real unlock invalidates that texture's snapshot**, so
a write the mod did not see cannot be served stale. Hooks arm behaviour only
after all four install, so a partial install is pass-through rather than
half-caching.

**Correction, from the high-resolution text study.** This paragraph used to say
the invalidation was needed "because the glyph atlas is a single mutable,
demand-paged surface", and reasoned about "a glyph paged in after the snapshot"
producing the Arland missing-kanji bug. The disassembly says otherwise. The
512×512 surface is **not a packed atlas at all** — it is a **one-glyph scratch**,
written at (0,0) and read back from (0,0), once per character per pass, with the
blit reading the mapping base with no glyph offset (`0x74c03a`, `mov rax, [rsp+0x28]`).

So the cache is correct for a stronger reason than paging stability: it serves
**both** the write and the read from the same buffer, which makes the writer and
the reader agree by construction. That is also why serving reads alone would have
been wrong, and why `snapshotDrops=0` is the expected result rather than a lucky
one. The unmatched-unlock invalidation is still right — it is what keeps a writer
the mod did not intercept from being papered over — but the "missing kanji under
paging" risk this section carried as unvalidated does not exist in this shape.

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

## The travel-map cursor

> **TL;DR**: Ayesha's travel-map cursor moved a fixed distance per rendered *frame*, so it crossed the map roughly three times too fast at 200 Hz. Same defect and same fix as Arland's Totori and Meruru. The driver and mover are located and prologue-gated in both Ayesha builds. **Validated in game.**

### The defect

`worldmap_fix.cpp` at EN `0x3376a0` reads the two stick axes, folds in the four
digital directions, rotates the result by the map heading, clamps to the map
bounds, and adds it to the cursor position. There is no `dt` term anywhere in
the addition. The immediate caller (EN `0x330c40`) *does* hold the real frame
delta — the sub-state dispatcher at `0x329c20` passes it in `xmm1` to every
registered callback — and simply never forwards it.

This is the same family as the field-jitter fix above and as Arland's
Totori/Meruru travel maps: a movement value applied per frame instead of per
second.

### The chain, and why two earlier searches missed it

```
WMGameMode::Update(this, dt)        0x306700   (vtable 0xd45048 slot 3)
 └─ WMStateMgr::Update(dt)          0x32a9a0
     └─ WMStateAutoMove::Update(dt) 0x32a5c0   (vtable 0xd46e60 slot 6)
         └─ sub-state dispatch      0x329c20   — forwards dt correctly
             └─ driver(owner, dt)   0x330c40   — holds dt, drops it
                 └─ move(self)      0x3376a0   — no dt parameter at all
```

Two failed searches preceded this, and both failures are reusable lessons:

1. **`callsites` on the axis accessor `0x1a0d00` returns nothing.** It is a leaf
   with no `.pdata` entry, and every caller reaches it through the
   incremental-link thunk `0xaa6f`. `callsites` on *the thunk* returns 53 real
   sites, the mover among them. This is the `.pdata` blind spot in
   `RE-PLAYBOOK.md` §5, hit for the second time in this project.
2. **The follow-up scan for "reads both axes and does an SSE normalize" returned
   zero because Ayesha's mover never normalizes.** Its step is `|stick| * speed`;
   it computes a length only to test it against zero, with a scalar `mulss`/
   `addss` chain and a call to `sqrtf`. The packed `rsqrtps` shape that Arland's
   movers have is not in this binary. Searching for the Arland *shape* rather
   than the Arland *behaviour* is what cost the second pass.

Identity is proven rather than inferred: the driver's thunk is `lea`'d inside the
constructor (EN `0x31fef0`), which installs vtable `0xd46e60` and zeroes the
`+0x120` input lock that both driver and mover gate on. That vtable's RTTI
complete-object locator is `0x114c7f0`, class **`WMStateAutoMove`** — a name
worth having, since "auto move" is exactly what this sub-state does. A first
pass called it `WMStateNormal`; that is a **different** sibling class with a
different layout (vtable `0xd46e08`, COL `0x114c760`), and the confusion was
caught by a second investigation and re-verified against `rtti` output. `homolog`
returns MATCH both ways for both functions (driver 18 votes to 2, mover 25 to 6),
and the ML build corroborates independently.

### Addresses

| | `Atelier_Ayesha_EN.exe` | `Atelier_Ayesha.exe` |
|---|---|---|
| driver | `0x330c40` | `0x33e770` |
| move | `0x3376a0` | `0x345470` |
| publish | inlined at `0x3378ea` | inlined at `0x3456ba` |
| `WMStateAutoMove` vtable | `0xd46e60` | `0xd4be80` |
| constructor / registrar | `0x31fef0` | `0x32d6d0` |

Both prologue windows are byte-identical across the two builds, so
`worldmap_fix.cpp` carries one pair rather than one per row.

### Struct offsets

`WMStateAutoMove` (`self`):

| off | meaning |
|---|---|
| `+0x28` | render node |
| `+0x30..0x3c` | cursor position, 16-byte-aligned `float[4]`; lane 3 is added to with zero |
| `+0x44` | current route/node id |
| `+0x120` | input lock; both driver and mover early-out when non-zero |

Render node (`*(self+0x28)`):

| off | meaning |
|---|---|
| `+0x38` | map heading fed to `MakeRotationY` |
| `+0xb0` / `+0xc0` / `+0xd0` | Vec3 previous / target / current |
| `+0xe0` | interpolation timer; `0` means snapped |

### The publish is inlined, so the fix reproduces it

The Arland movers call a separate publish helper, which the Arland fix simply
calls again with the corrected position. **Ayesha has no such function.** The
publish is inlined into the mover at `0x3378ea..0x33794c`, and three independent
searches for an out-of-line copy — the exact `movss` store sequence, both `mov`
register forms, and Arland's `movaps`/`movdqa` forms — found seventeen inlined
sites and no leaf. The only leaf-shaped candidate, `0x31c6ef`, is a *getter*.

So `republish()` writes the four fields directly: previous, target and current
all get the corrected Vec3, and the timer is stored as zero. That is what the
mover itself does, so it changes the values without changing the state, and
`WMStateAutoMove::Enter` (`0x333c20`) reads the initial position back out of
`node+0xd0`, which keeps both authorities consistent. One `readableRange` check
spans the whole `+0xb0..+0xe4` block so a stale node pointer cannot leave it
half-written.

### The correction

Hook the driver to capture `dt` into a `thread_local`; hook the mover, snapshot
the position before, and afterwards rescale the delta it produced by
`clamp(dt * 60, 0, 1)`. At 60 fps and below the factor is 1 and shipped
behaviour is preserved bit-for-bit. Nothing is predicted or simulated — the
mover runs untouched and its output is scaled — which is why this cannot
desynchronise from anything else that reads the position. Interpolating back
toward the previous position cannot escape the bounds the mover just clamped
into, because an axis-aligned box is convex.

The mover returns early without touching the position when nothing is held, so
the correction keys on a non-zero measured step rather than on the return value.

### Gating

Ayesha-only and OnByDefault, on the same reasoning as the field-jitter fix.
`DUSK_WORLDMAP=0` turns it off for a comparison; `DUSK_WORLDMAP_PROBE=1` logs
`raw_per_s` against `applied_per_s`, which is the whole measurement — the first
should scale with refresh rate and the second should not. Escha & Logy and
Shallie are `Unsupported` because their travel map has not been looked at, not
because it is known to be fine.

### Confirmed in game

**Validated on Ayesha** — the cursor moves at the speed the game was built for at
200 Hz. The fix ships `OnByDefault`; `DUSK_WORLDMAP=0` turns it off for a
comparison and `DUSK_WORLDMAP_PROBE=1` logs `raw_per_s` against `applied_per_s`.

One operator note, because it cost a run and it is the second time this exact
mistake has been made in this project: the first test showed no change because
the build was never **deployed**. Building is not deploying. `FIXES
world_map=active driver_rva=0x... move_rva=0x...` in the log is the only proof
the module installed; `world_map=unavailable` means an undeployed or older DLL,
and `world_map=declined (prologue mismatch)` would mean the addresses are wrong,
which is a different problem entirely. Check that line before judging the cursor.

### Flagged, unresolved: a second dt-free mover with no callers

`0x310540` (world-map camera zoom) has the identical dt-free `+=` shape, writing
`+0x78/+0x7c/+0x80` and zeroing `+0x84` — the scalar form of the same
interpolator block. But it has no callers: its only reference is the thunk
`0x1c2a1`, which has no call or jump sites and is never `lea`'d, and a raw search
for both VAs finds nothing. It looks like dead code. Recorded so nobody mistakes
it for a live second defect without runtime evidence.

## The system-save wipe

> **TL;DR**: Escha & Logy and Shallie lose their system save — settings, gallery, costumes, bonus flags — and the cause is not the writer. A failed *load* reports success, a zero-filled buffer is installed over the live data, the deserializer accepts it silently, and the next settings change writes the defaults back over a healthy file. Four independent missing checks. All four KTGL builds are affected. **Defect reproduced and fix validated in game.**

### Where it lives

`%USERPROFILE%\Documents\KoeiTecmo\Atelier Escha and Logy DX\SAVEDATA\SYSDATA.pcsave` — confirmed on disk in the test prefix (2960 bytes, high entropy) and in the binary.

**The filename is a wide literal**, which is why a narrow-string grep finds nothing: Escha EN `.rdata` `0x7ceaf8` = `L"SYSDATA.pcsave"`, `0x7cead0` = `L"\SAVEDATA\"`, `0x7ceb30` = `L"%ls%ls%ls"`, base-dir global `0x10c6740`. Path builders at `0x138730` (the directory), `0x138780` (the system save), `0x1387d0` (`GAMEDATA%02u.pcsave`).

**No Win32 file API is used for saves.** The writer is CRT stdio — `_wfopen_s`, `fwrite`, `fread`, `ftell`, `fclose`. Directory creation is `SHCreateDirectoryExW`, existence `PathFileExistsW`, free space `GetDiskFreeSpaceExW`. There is **no `rename`, no `MoveFileW`, no `ReplaceFile`, no `FlushFileBuffers`** anywhere on this path: no temp-file-and-rename, no atomic replace, no backup.

The class layer is `PlatformSteam::Save` / `::Load` / `::Exist` / `::Delete`, each a two-slot vtable `{dtor, run}` over a shared 0x438-byte object:

```
+0x10  state          +0x14  errcode        +0x18  wchar path[0x200]
+0x418 isSystemData   +0x420 buffer         +0x428 size / bytes read
+0x430 "completed" flag
```

### The defect is in the load, not the write

Four things had to line up. Any one of them alone would have prevented the loss.

**1. Open failure reports success.** `PlatformSteam::Load::step`, Escha EN `0x138df0`:

```
0x138e16  call [rip+0x5e0bd4]      ; _wfopen_s(&fp, path, L"rb")
0x138e1c  test eax, eax
0x138e1e  jne  0x138ecb            ; OPEN FAILED    ->
0x138e29  test rdi, rdi
0x138e2c  je   0x138ecb            ; fp == NULL     ->
   ... read ...
0x138ead  test dil, dil
0x138eb0  jne  0x138ecb            ; READ SUCCEEDED ->
0x138eb2  mov  dword [rbx+0x10], 7 ; read failed: state 7, err 5
0x138eb9  mov  dword [rbx+0x14], 5
0x138ecb  mov  qword [rbx+0x10], 6 ; <-- SHARED EXIT: state 6, err 0
0x138ed3  mov  byte  [rbx+0x430], 1;     completed = TRUE
```

The open-failure branches land on the **same exit as success**, which sets the completion flag. The qword store there also clears the `-1` the constructor wrote to `+0x14`. A transient open failure on an existing, healthy file is indistinguishable from a clean load, and `+0x428` is still the constructor's zero.

The genuine file-not-found case is handled correctly and separately (state 1 checks `PathFileExistsW`, sets state 6 / err 4 / flag 0), so this is not the first-run path.

**2. A zero-byte or short file also reports success.** The read is `fread(buf, 0x5000, 1, fp)`, which returns 0 for any file smaller than 0x5000 — that is, always. The code treats that as normal, recovers the real length with `ftell`, and unconditionally sets success. A zero-byte file yields `bytesRead = 0` and `completed = 1`.

**3. The caller installs the zeroed buffer.** System-load thread, Escha EN `0x15d4e0`: allocates a 0x5000 scratch buffer, `memset`s it to zero, runs the Load, then

```
0x15d5c6  mov  esi, [rbx+0x428]     ; bytes read (0 in the failure case)
0x15d5cc  cmp  byte [rbx+0x430], 0
0x15d5d3  je   0x15d672             ; not taken - flag is 1
0x15d5ee  xor  r9d, r9d             ; codec out-size pointer = NULL
0x15d5f7  call 0x219a0              ; decode; RETURN VALUE DISCARDED
0x15d608  call 0x26610              ; vector::resize(sysdata, 0x5000)
0x15d620..0x15d670                  ; copy 0x5000 bytes
```

With `size == 0` the codec does nothing, so the entire zero-filled scratch buffer is copied over the live system-data vector. The codec *does* carry an integrity check (`cmp byte [r13+r15], 0xff` at `0x21c14`) — but the caller passes NULL for the out-size and discards the return value, so nothing can act on it.

**4. The deserializer accepts an empty blob silently.** `0x40fa0` → `0x411d0` walks a list of chunk readers, each calling `findChunk` (`0x35b320`). A reader whose chunk is absent simply advances to the next, and the function returns success. An empty vector returns success too. Only a C++ exception produces the failure codes that would raise the `MessageBoxA` "system ロードに失敗しました。err: %d" (`0x752880`).

So a zero blob deserializes cleanly as "no chunks present", the live object keeps its default-constructed values, **and no error is ever shown** — which is exactly the reported symptom.

### And then it is written back

`saveSystemData`, Escha EN `0x40730`, serializes the live object, pads to 0x5000, and requests an async save. Its only early-out is a serializer error. **There is no "was the system data ever loaded?" guard anywhere on the path.** Three places could have caught the failure — the completion flag, the codec return, the chunk deserializer — and none do.

Save triggers, by vtable slot: `OptionBase`/`OptionMenu`/`OptionOther`/`OptionTitle` slot 8 → `0x117c80` → `0x40730`; `ClearSaveGameMode` slot 3 → `0x37c50`; `ScreenControl` slot 1 → `0x15db40`. The Options one matches the community observation precisely — the data is lost only if you actually change a setting after a failed load.

### Where the zero-byte file comes from — inferred

`_wfopen_s(path, L"wb")` truncates the file to zero at the moment of opening, before a byte of new content exists, with no temp file and no backup. Two ways that becomes permanent:

1. **Process exits mid-write.** The save is dispatched to a detached worker (`0x15ccb0` creates it, body `0x15d380`), and `saveSystemData` returns immediately. The thread is joined only at the *start of the next request* (`0x15d8f0`) — **never at shutdown**. The screen-state machine normally polls to completion (`0x13fe10`), but `OptionBase::apply` starts the save and returns, leaving the poll to the option screen's update loop. A quit taken in that window leaves a truncated file.
2. **Transient open failure at load.** Steam Cloud syncs `SAVEDATA` on launch and exit; a backup tool or scanner holding the file gives `_wfopen_s` an error, which point 1 above converts straight into a silent zero. This explains the "random" quality better than anything else in the code.

Both funnel into the same install-and-overwrite chain. **Unresolved:** no explicit "save system data then exit" call was found in the shutdown path — the quit sequence itself is not mapped. That does not weaken the mechanism, since route 2 needs no quit-time save at all, but the specific "on quit" trigger is not pinned.

### Shallie is affected identically

All four builds share the defect byte-for-byte at the shared exit, despite Shallie's save layer being refactored (its `PlatformSteam::Base` factors the fopen/fread wrappers out instead of inlining them, so it has one `SYSDATA.pcsave` xref where Escha has five). Object layout, state machine, shared exit and unconditional install are the same.

| | Escha EN | Escha ML | Shallie EN | Shallie ML |
|---|---|---|---|---|
| `Load::step` — the bug | `0x138df0` | `0x13fa60` | `0xc2670` | `0xc3ec0` |
| `Save::step` — the writer | `0x138a70` | `0x13f6e0` | `0xc28d0` | — |
| system-load thread | `0x15d4e0` | `0x164fe0` | `0x287020` | — |
| system-save thread | `0x15d380` | `0x164e80` | `0x286ed0` | — |
| `saveSystemData` — no guard | `0x40730` | `0x45ba0` | `0xd7610` | — |
| shared exit | `0x138ecb` | `0x13fb3b` | `0xc270f` | `0xc3f5f` |

### Reproduced, and the fix validated

**2026-08-02, Escha & Logy EN.** `SYSDATA.pcsave` was backed up and truncated to 0 bytes, then the game was launched with the guard active:

```
[  17] FIXES system_save=active load_rva=0x138df0 save_rva=0x138a70
[1644] SYSSAVE load reported success having read 0 bytes -- forced to the
       engine's read-failure state, and system-data saves are now refused (n=1)
[1856] SYSSAVE refusing system-data save ...
```

Three things are established by that run, and the first two are about the *defect* rather than the fix:

1. **Mechanisms 1 and 2 are observed, not inferred.** The game read a zero-byte file, declared success and set its completion flag. No error dialog appeared, which also confirms mechanism 4 — the deserializer accepts an empty blob silently.
2. **The destructive write was refused, and the file survived.** `SYSDATA.pcsave` was still 0 bytes afterwards. Without the guard it would by then have been a valid ~3 KB file of defaults, which is the irreversible half of the defect.
3. **`GAMEDATA00.pcsave` was byte-identical to its backup.** This was the outcome most worth checking: both save kinds share `Load::step` and `Save::step`, and a wrong `isSystemData` test would have blocked ordinary game saves. It did not.

The 212-tick gap between the two `SYSSAVE` lines is the shape of the defect in the wild — the load fails silently at startup, and the overwrite happens later, only when the player touches the Options screen.

### What to measure

Static work is done; three cheap checks close it, and the first is decisive.

1. **Reproduce the silent zero load.** Back up `SYSDATA.pcsave`, truncate it to 0 bytes, launch. Expected: the game reaches the title with **no error dialog**, and Options, gallery and costumes read as factory default. Change one option and quit — the file is now a valid ~3 KB save of defaults. If a failure dialog appears instead, point 4 is wrong and the deserializer does reject empties.
2. **Reproduce the open-failure branch.** Hold the file open with an exclusive deny-read lock while launching. Same expected outcome; this distinguishes "zero-byte file" from "unreadable file" as the real-world trigger.
3. **Confirm the truncation window.** Change an option to force a save, then quit within about a second. If the file is ever left at 0 bytes, the shutdown race is live.

### The fix, when it comes

Narrowest first:

- **`Load::step`**: split the shared exit so the `_wfopen_s`-failure and `fp == NULL` entries go to the existing state-7 branch, leaving `+0x430 = 0`. Four RVAs, one per build, all clean `.pdata` function starts.
- System-load thread: refuse to install the decoded buffer when `[obj+0x428] == 0`.
- `saveSystemData`: refuse to save when the last system load produced nothing.

None need a new file format and none touch on-disk data. An external backup — what AELBackup does — remains worth having regardless, because the write path has no atomic replace at all.

### Two corrections to earlier notes

- These binaries **do** import `GetPrivateProfileSectionW` and `GetPrivateProfileSectionNamesW`. The earlier claim was about `GetPrivateProfileInt`/`String`, which are genuinely absent, but "imports no `GetPrivateProfile*`" would be wrong. The custom key/value reader is still where `Setting.ini` is read, and the system save is a wholly separate mechanism.
- `nosteam/games/05-escha/Saves/` is **game data** (`Bonus.xml`, `SysMess.xml`, `WorkData.xml`), not save data. Nothing user-writable lives in the game directory.

## KTGL antialiasing feasibility

> **TL;DR**: Both KTGL games are **forward** renderers with a single scene colour + depth pair, so the twin-resource MSAA design ports over. The engine probes for multisampling and then passes a literal zero everywhere, and never calls `ResolveSubresource` at all. Cost is one new hook slot and a scene test whose size predicate is much weaker than Ayesha's.

### Forward, verified by census rather than impression

Every DXBC container under `Data/x64/Shader/*.g1s` (Escha 853, Shallie 1050):

| | Escha | Shallie |
|---|---|---|
| PS writing 1 `SV_Target` | 136 | 181 |
| PS writing 0 `SV_Target` | 17 | 18 |
| **PS writing >= 2 `SV_Target`** | **0** | **0** |

**No MRT pixel shader exists in either game** — there is no G-buffer writer — and **no `texture2dms` declaration anywhere**, so no shader is written to read a multisampled resource. The zero-target shaders are exactly the shadow casters plus Shallie's `RenderTerrainShadowMap`, not a depth prepass. Bind names are a pure forward material set (`sDiffuse`, `sSpecular`, `sToon`, `sShadow`, `sLit`, ...), and `MapSTD`'s pixel shader does lighting and the 16-tap PCF inline.

The post chain confirms it independently: `Data/x64/PostEffect/pe_pack.elixir.gz` holds 49 (Escha) / 52 (Shallie) fullscreen passes, every one single-target, every one sampling `texScene` and nothing else beyond a glow ladder and a capture texture. Shallie's DOF pass is a 2-tap blur with **no depth input**. There is no deferred lighting pass.

### The engine's MSAA plumbing is complete and never engaged

The graphics layer is byte-identical between the two games. The probe is real — device vtable **slot 30** (`+0xf0`, taken from the MinGW header), three call sites per binary — and walks AA levels 0..2 across 125 formats, storing `min(quality)-1` into `renderer+0x3070/3074/3078`. The sample ladder is `1, 2, 4, 0`, identical to Ayesha's. The view creators are MSAA-aware, picking RTV/DSV dimension 6/7 (`TEXTURE2DMS`) off the descriptor's sample count.

**But the AA level is a hardcoded literal zero at every scene-target creation**, traced argument by argument through the whole factory chain: the scene pair, the shadow pair, the general texture factory and Escha's extra pool all pass a freshly-zeroed register.

**And neither binary calls `ResolveSubresource`.** Exhaustive byte search for context slot 57 finds exactly two hits per binary and both are ordinary C++ virtual calls on game objects.

That kills the cheap alternative: forcing the engine's own AA level would produce multisampled targets that nothing resolves, sampled through `Texture2D` declarations. **The mod has to do it, exactly as on Ayesha.** There is no live setting either — `Setting.ini` carries only `ScreenWidth`/`ScreenHeight`/`Outline` (plus `Shadow` on Shallie).

### Bind flags are computed, not literal — and that is the discriminator

This resolves an "unresolved" left by the shadow investigation, which correctly found no `0x40`/`0x48` literal in either binary. The flags are computed by two sibling factories:

- **`0x3d3f70`** (Shallie `0x3ad250`): `cmovne` picks `0x20` or `0x40`, then **`or eax, 8` unconditionally** → `0x28` / `0x48`.
- **`0x3d4110`** (Shallie `0x3ad3f0`): the same `cmovne`, and **never ORs in `SHADER_RESOURCE`** → `0x20` / `0x40`.

This cross-validates AGT's runtime-observed shadow descriptor (1024², format 44, BindFlags `0x40`) by a second, independent method: the shadow depth is allocated through the only factory that cannot produce an SRV.

**Side finding that revises the shadow item.** The shipped Escha **1.0.0.1** binary still routes its shadow depth through `0x3d4110`, so it still creates it `DEPTH_STENCIL`-only. Whatever patch 1.01 changed, **it was not the shadow depth's bind flags.**

### The scene pair, and the rule

Both games, from `0x505260` / `0x566280`:

| | colour | depth |
|---|---|---|
| size | swap-chain size | swap-chain size |
| BindFlags | **0x28** `RENDER_TARGET \| SHADER_RESOURCE` | **0x48** `DEPTH_STENCIL \| SHADER_RESOURCE` |
| format | typeless BGRA8 (90, inferred) | `R24G8_TYPELESS` (44, inferred) |
| mips / slices / samples | 1 / 1 / 1 | 1 / 1 / 1 |

The rule is Ayesha's with "colour is typeless" swapped for "colour is exactly RT\|SRV". As on Ayesha it must be a **pair** test; neither half is sufficient.

What else in the frame matches:

- **Shadow pair** — excluded twice: wrong size *and* depth is `0x40` not `0x48`. The naive rule the brief warned about is defeated by the depth predicate alone.
- **Back buffer** — `0x20` from the swap chain; fails the colour predicate.
- **The real weak point.** Unlike Ayesha, which pins its scene to 1920×1080 while everything else is another shape, **KTGL allocates its scene targets at swap-chain size** — so `matchesSwapChain` is true for the scene pair, the back buffer, the UI composites, and **in Escha only** three further screen-size `0x28` colour targets from a ping-pong pool at `0x50c040`. Those satisfy the colour predicate and are excluded only while never bound alongside the `0x48` depth. **Shallie has no equivalent** — `homolog` cannot find that pool in it at all.

So the existing `MSAA: WARNING a second distinct target pair matched the scene test` diagnostic is **load-bearing on this engine**, not decoration. And the descriptors being identical between the two games does not mean the allocators are: Escha has a whole extra render-target pool Shallie lacks.

### What would break the twin design

**(a) Compute-shader reads of the scene — a new hook is required.** The particle/effect compute shaders embedded in both executables bind the scene colour *and* depth (`texSource`/`texDepth`, `texDepth`/`texNormal`). `msaa.cpp` hooks `PSSetShaderResources` only; it needs **`CSSetShaderResources`, context slot 67 (`+0x218`)**. Without it a compute pass samples an unresolved host and `depthHostReads` reads **zero while depth is being read** — precisely the silent-success failure the rewrite exists to prevent.

**(b) The scene depth carries `SHADER_RESOURCE` in both games** (`0x48`, unlike the shadow depth's `0x40`), so the depth-host-read question is live rather than theoretical. `ResolveSubresource` rejects depth formats, so a non-zero `depthHostReads` means the shader depth-resolve pass has to be written. Note TellowKrinkle's `impl.cpp` strips `SHADER_RESOURCE` off its depth twin — that would be wrong here.

**(c) `CopyResource`/`CopySubresourceRegion`** are already hooked, so covered. The earlier "~9 KTGL-only `CopyResource` sites" figure should be treated as **unconfirmed**: most byte hits are ordinary C++ virtual calls on game objects, not the context.

**(d) UAVs.** `0x3d3f70` can set `BIND_UNORDERED_ACCESS` and one thunk forwards that argument. The accepted risk "UAVs are not saved across a resolve" should be re-checked from the census — look for a scene-size line whose bind flags contain `0x80`.

**(e) Static call-site enumeration of context methods does not work on this engine.** `CSSetShaderResources` has *zero* direct call sites despite the shaders needing it, because KTGL dispatches per-stage setters through computed tables. Vtable hooking is unaffected, but "I found N call sites" is never an upper bound here.

### The one run that settles the rest

`DUSK_TARGET_CENSUS=1`, one session that **reaches a field map** rather than just the title screen:

1. `samples=` on every line should be `1`. Any `2`/`4` falsifies "the engine never multisamples".
2. The scene pair: two `matchesSwapChain` lines, one `bindFlags=0x28 format=90`, one `bindFlags=0x48 format=44`.
3. **How many distinct shapes carry `bindFlags=0x28` at swap-chain size** — expect ~1 in Shallie and ~4 in Escha. That count is the direct measure of how ambiguous the scene test is, per game.
4. The shadow pair, which also re-confirms that 1.01 did not widen the bind flags.
5. Any scene-size line with `0x80` in the bind flags.
6. `callerRva` will not discriminate here — every texture funnels through one wrapper. The shape has to carry the rule.

### Cost

Not needed: shader replacement, G-buffer handling, or any change to the resolve model. Needed: a `MsaaSceneTest` in `src/engines/ktgl/` mirroring `scene_target.cpp`; **one additional hook slot, `CSSetShaderResources` (67)**; and a decision on the depth-resolve pass once `depthHostReads` is measured *with the CS hook in place*.

## High-resolution text: Ayesha

> **TL;DR**: There is no rasterizer to re-target — the font is a pre-rendered bitmap asset. What is reachable is Arland's approach: upscale the engine's own composed string bitmap. And it is *easier* here than in Arland, because everything downstream of the bitmap is normalized, so the most fragile piece of the Arland implementation is not needed.

### There is no font rasterizer

- **No font APIs.** `GDI32.dll` imports exactly one symbol, `GetStockObject`. No `CreateFontIndirect`, no `GetGlyphOutline`, no `AddFontMemResourceEx`, no DirectWrite, no FreeType. Identical in both builds.
- **No embedded font.** Zero `glyf` and zero `cmap` tag occurrences; no sfnt/OTTO/ttcf table directory.
- **The shipped asset is a baked bitmap font.** `Res/x64/font/` holds only `ArlandDX_font_jp.g1n` and `ArlandDX_font_ch.g1n` — and the `_ch` file is **md5-identical to Rorona's, Totori's and Meruru's**. The filename says `ArlandDX`: Gust reused the Arland DX text pipeline wholesale.
- **G1N parsed**: magic `_N1G0000`, sub-image count at `+0x18` (jp 152, ch 175), each carrying a 16-entry palette of the exact form `ffffff00, ffffff11 … ffffffff` — white with a 16-step alpha ramp — over uncompressed 4-bit alpha.
- **The TTF/OTF paths are dead legacy.** `FOT-NewCinemaAStd-D.otf`, `Uhei00m.ttf`, `bGTR00B.ttf`, `Tuffy.ttf` appear as `.rdata` strings in a language-indexed table at `0x1487ca0`, but **`Res/font/` does not exist in the install** and no font file ships anywhere. Rorona and Meruru carry the same dead strings — PhyreEngine's `PFontWin` path, orphaned by the DX port.

**So "rasterize at display resolution" is impossible without supplying a new font.** That closes the obvious framing outright.

### Size is baked, not derived from resolution

Inside `renderText` (EN `0x74bd90`, ML `0x76e290`) a single `cmove` picks a glyph cell height of **48 or 32 pixels** off a flag at `+0x1c8`; neither branch reads the display size. Per-glyph advances are **hard-coded integers** built into a `(codepoint, width)` table by `0x748320` (ML `0x76a820`, MATCH 87 votes), and all layout is integer pixel arithmetic.

There is **no fixed virtual canvas** the way KTGL has one — the text quad is placed by a transform and sized as `height × aspect`. So there is no clean `display / canvas` factor to attack. This does not affect feasibility; the design below is scale-agnostic.

### The 512×512 surface is a one-glyph scratch

This is the finding that reshapes the item, and it corrects the atlas-cache section above.

`renderText` runs each string **twice** — pass 0 measures and allocates, pass 1 rasterizes and blits — which is exactly the census's `27,568 / 13,784 = 2.000` write:read ratio. Per character, pass 1 does one write-begin, one read-lock, one blit, one unlock. And the blit reads **the mapping base with no glyph offset**:

```
0x74c020  call 0x3abc                 ; lock -> eax = RowPitch, *out = pData
0x74c03a  mov  rax, qword [rsp+0x28]  ; the mapped base, unmodified
0x74c06e  movzx ecx, byte ptr [rax+3] ; alpha of a BGRA texel
0x74c072  mov  byte ptr [r8+rdx], cl  ; -> 8bpp string bitmap
```

So each glyph is decoded into the top-left of the surface and read straight back. **"Rasterizing at 2× needs 4× the atlas area" does not apply** — a 48×48 cell in a 512×512 surface has ~113× headroom, and there is no packing or paging problem to solve. The mod's `isMutableFontAtlas` 512×512 assumption stays valid, since the surface shape does not change.

### Everything downstream is normalized — which removes Arland's most fragile hook

`renderText` rounds the composed extent up to a power of two and returns `potW`, `potH`, an 8bpp buffer, and four **fractions** (`usedW/potW`, `usedH/potH`, `lineHeight/potH`). The consumer `setText` (EN `0x74db60`, ML `0x770060`, MATCH 37 votes) creates a texture at exactly `potW × potH`, expands 8bpp alpha to BGRA, frees the buffer with the engine's own free, writes four **normalized** UV pairs, and stores **only an aspect ratio** at widget `+0x348`:

```
0x74de77  mulss xmm1, xmm6      ; potW * (usedW/potW)
0x74de83  mulss xmm0, xmm7      ; potH * (usedH/potH)
0x74de8c  divss xmm1, xmm0      ; usedW / usedH
```

`+0x348` is read only as `width = height × aspect`, where the height comes from the widget rather than from `potH`.

**So a uniform k× of the bitmap changes nothing on screen.** UVs are fractions and the aspect is invariant under uniform scaling. Arland needed `hiResTextRestoreDims()` because its auto-size widgets read the raw dimensions back; on the static evidence **Ayesha does not need that hook at all**. That is the largest difference from Arland and it removes its most fragile piece.

`setText` is the single funnel — 16 confirmed callers through thunk `0x4ae3` — and `renderText` has exactly three entries, so hooking `renderText` covers all of them.

### Addresses this study establishes

| anchor | Ayesha EN | Ayesha ML | evidence |
|---|---|---|---|
| `setText` | `0x74db60` | `0x770060` | homolog MATCH 37/0, prologue identical |
| `setText` thunk | `0x4ae3` | — | 16 confirmed callers |
| glyph metrics / prepare | `0x748320` | `0x76a820` | MATCH 87/1, size `0x2827` both |
| text-bitmap alloc (thunk) | `0x2d9510` | `0x2e4800` | EN verified; ML inferred from byte shape |
| text-bitmap free | `0x2d93b0` | `0x2e46a0` | MATCH 20/0 |
| font-path table (dead) | `0x1487ca0` | — | 5 entries, none of which ship |

Thunk resolution cross-checks the existing hook map exactly: `0x3abc -> 0x581420` (lock), `0x1163 -> 0x581460` (unlock stub), `0x136b -> 0x581470` (unlock impl).

### Designs, ranked

**A — upscale the engine's own composed bitmap. Recommended.** Hook `renderText`; on return, allocate `2*potW × 2*potH` through the engine allocator, bilinear-upscale with alpha-coverage steepening, free the old buffer, write back. Keep all four fractions. **No consumer hook and no dimension restore.** Preserves layout, alignment, multi-line and the game's icon glyphs exactly, because the content is the engine's own. It buys smoothness, not detail — a 48 px glyph with 16 alpha levels has no more information to recover. `atelier-arland-fixes/src/font_hires.cpp` already contains this code; the port *removes* a hook rather than adding one.

**B — re-render from a bundled scalable font.** Genuine detail, but it must honour the hard-coded integer advances rather than the font's own metrics, or line breaks move. Blocked on the multilingual build by CJK coverage. Medium risk on EN, high on ML.

**C — render text to a higher-resolution overlay.** Rejected: the UI is transformed quads composited into the scene, not a separate 2D pass, and it would collide with the SSAA back-buffer redirect and the high-res target substitution.

**D — force the 48 px variant everywhere.** Rejected: the advance table is size-dependent, so this changes metrics, which means it changes layout.

### What would falsify it

1. **The scale-invariance claim itself** — the cheapest and most important test. Substitute doubled dimensions while leaving the bitmap content alone. Text at the same size (garbled but the same box) confirms the analysis; text at double size means an absolute-pixel reader exists and Arland's restore hook must be ported after all. Five lines, one run.
2. **Aspect drift.** `+0x348` must be bit-identical with the feature on and off for a fixed string.
3. **Multi-line and alignment** — save-slot lists, item descriptions, quest text, the synthesis recipe list. Line breaks must fall on the same words.
4. **Icon glyphs**, which are glyphs in the same G1N.
5. **Language switch and the ML build** — a kanji-heavy string, Simplified and Traditional Chinese, and RSS over a long session.
6. **Interaction with the atlas cache.** Design A runs *after* `renderText`, so atlas traffic must be unchanged and `snapshotDrops` must stay 0. Measure with the cache off and on.
7. **Memory ceiling.** `potW` is a power of two with no upper clamp, so the substitution must refuse above a byte budget as Arland's does.

### Licensing

The G1N carries no name table, so the baked face is not identifiable from the asset. The dead path strings name proprietary PS3-era faces. **Design A never ships a font**, which is most of the reason to prefer it. Design B's coverage bar is set by the multilingual build — Latin-1, full-width ASCII, CJK brackets, Roman numerals, kana, kanji, Simplified and Traditional Chinese — where the only realistic openly licensed answer is Noto Sans CJK / Source Han Sans (SIL OFL) at roughly 20 MB per weight per variant. That is why the Arland project wired substitution to the **English builds only**, and the same restriction should be the default here.

## The KTGL shadow subsystem

> **TL;DR**: Escha's shadow depth buffer is created without `BIND_SHADER_RESOURCE`, so the receiver reads the shadow pass's *colour* attachment instead — which the alpha-tested caster shaders never write. Result: rectangular block shadows under foliage. **But the shipped Escha binaries are patch 1.01, which reportedly fixed it.** Shallie was never patched and has a different, independent half of the same broken subsystem.

### What AGT does, read as behavioural evidence only

AGT is closed and unlicensed; what follows is a description of its observable behaviour, used to locate the engine defect. Nothing from it is copied.

**Game gating.** A 36-entry `(exeName, flagIndex, flagValue)` table at RVA `0xa9d10`, matched with `CompareStringEx`, writes `byte[0x187120 + flagIndex]`. Escha maps to index 1, Shallie to index 2. The shadow-view hack is gated on `byte[0x187121]` — **Escha only**. On an unrecognised executable it writes `0x101` to that word, which enables both the Escha *and* Shallie hacks by default.

**Step 1 — `CreateTexture2D` hook (`0xd850`)**, matching one exact descriptor:

| field | value |
|---|---|
| Width / Height | `0x400` / `0x400` |
| MipLevels / ArraySize | 1 / 1 |
| Format | `44` = `R24G8_TYPELESS` |
| Usage | `DEFAULT` |
| **BindFlags** | **`0x40` — `DEPTH_STENCIL` only** |

It ORs in `BIND_SHADER_RESOURCE`, calls through, and builds an SRV with format `46` (`R24_UNORM_X8_TYPELESS`), dimension `TEXTURE2D`, one mip.

**Step 2 — `OMSetRenderTargets` hook (`0xeaf0`)** identifies the shadow pass by both attachments at once: DSV resource 1024² format 44 bindFlags `0x48` (the one it just patched), RTV resource 1024² format `90` = `B8G8R8A8_TYPELESS` bindFlags `0x28`. It caches the **colour** resource.

**Step 3 — `PSSetShaderResources` hook (`0xf0a0`)** substitutes its depth SRV whenever an SRV over that cached colour resource is bound.

So: the game renders its shadow pass into a depth buffer it cannot read plus a colour attachment, and binds the colour attachment as the shadow map.

### What is actually wrong, from the shaders

KTGL ships raw DXBC inside `Data/x64/Shader/*.g1s` with no container obfuscation (853 containers on Escha, 1050 on Shallie). Read with `tools/g1s.py` and `tools/sm5dis.py`.

**The receivers** (`MapSTD*`, `STD*`, `MapWater`) bind `sShadow` at t1 with `__smpsShadow` at s1, declared `texture2d` returning `float`, and do a **16-tap manual PCF**:

```
dcl_sampler mode=0 samp1                     ; ORDINARY sampler, not comparison
sample_l  r4.x, r3.zwzz, res1, samp1, l(0)   ; x4 per quad, x4 quads
lt   r3, r4, r0.yyyy                         ; the comparison, in the shader
movc r4, r3, l(0,0,0,0), l(1,1,1,1)
```

They want a single-channel depth in `.x`, sampled with an ordinary sampler at LOD 0, and do the depth test themselves. **Across both games there are zero comparison samplers and zero `sample_c`/`sample_c_lz` instructions** (294 and 484 `dcl_sampler` respectively). Nothing in either game ever wants hardware PCF.

**The casters.** All 17 `SM*_Psm` pixel-shader variants have **no output signature at all** — no `SV_Target`. They exist purely to run an alpha-test `discard`, and write only depth. `SM_Psm.g1s` is md5-identical between the two games.

**The one shader that writes a colour shadow map** is `RenderTerrainShadowMap.g1s`, and the games differ:

- **Escha** writes depth into colour with a slope-scaled offset: `o0 = (v1.x + cb0[0].x*(|ddx| + |ddy|)) + cb0[0].y`
- **Shallie** is `dcl_globalFlags; ret` — writes nothing.

Shallie also ships `DepthShader.g1s`, an RGB-packed depth writer, which Escha lacks.

**The consequence.** Alpha-tested casters populate only the depth buffer; the non-alpha-tested terrain shader populates the colour buffer; the receiver reads the colour buffer. **Foliage casts its full untextured quad.** The engine's RTTI carries `ktgl::VCShadowMapShader::CBlendMapTerrainShadowMapShaderBase`, so a separate no-alpha-test terrain shadow shader is a real, distinct thing in this renderer.

That matches the AGT author's public description exactly: "boxy leaves on trees", branch-only rendering on AMD, "the PS3 version has proper leaf rendering in its shadows", plus missing grass patches — and the technical claim that the shadow texture was "allocated as a texture that cannot be used as a resource".

**Predicted symptom for a confirming run:** tree shadows on open ground in daylight. Broken = solid rectangles under foliage, or foliage shadows missing entirely. Correct = leaf-shaped dappling.

### Two halves, not one defect

The Shallie sampler item is the **same subsystem and a different breakage**. AGT's `CreateSamplerState` hook (`0xd680`) matches `Filter == 0x94`, CLAMP×3, MaxAnisotropy 1, `ComparisonFunc = LESS_EQUAL`, LOD collapsed to `[0,0]`, and rewrites the filter dword to `0x14`.

That sampler is now identified: it is **`__smpsShadow`**, the s1 sampler of the manual-PCF receiver above. Every field matches what the shader needs *except* the comparison bit — CLAMP×3 is a shadow map, MaxAnisotropy 1 and LOD `[0,0]` match `sample_l … l(0)` on a single-mip 1024² map, and `LESS_EQUAL` is the test the shader open-codes with `lt`. Binding a comparison sampler to a non-comparison `sample_l` is undefined in D3D11 and returns 0 on essentially every driver, so **all 16 taps read 0**, `lt(0, ref)` is uniformly true, and the ground reads **uniformly dark with no shadow shapes** — a visibly different symptom from Escha's blocky one.

The engine can express this deliberately: Escha's sampler-key decoder `0x3d6c40` reads an 8-byte key where `byte[2] bit 3` is the comparison flag and bits 4-6 the comparison func, and the filter mapper `0x3e0040` applies it with `bts eax, 7`. AGT's descriptor decodes to key bytes `15 14 38 00 00 00 00 00`. **Unresolved:** that key appears as a literal nowhere in either executable or in ~27,000 game data files, so it is assembled at runtime.

Fixing one half would not fix the other, and they should stay separate items.

### Escha is probably already fixed upstream

| binary | FileVersion | PE link timestamp |
|---|---|---|
| `Atelier_Escha_and_Logy_EN.exe` | **1.0.0.1** | 2020-10-21 |
| `Atelier_Escha_and_Logy.exe` | **1.0.0.1** | 2020-10-21 |
| `Atelier_Shallie_EN.exe` | **1.0.0.0** | 2019-12-26 |

The AGT author's own Steam thread carries an October 2020 report that patch 1.01 fixed shadows, later confirmed with screenshots — and the local copies are that build. Suggestive but not proof: if 1.01 had changed the bind flags, AGT's matcher would stop firing and it would pop "unable to replace SRV as it wasn't created on init!" on every launch. Nobody reported that, and nobody reported the contrary.

**Inferred, not verified.** Which side the patch changed could not be established statically: the bind-flag argument to the KTGL texture wrapper (Escha `0x3d3c10`, device at `this+0x3048`) is a variable at every reachable call site, and no `0x40`/`0x48` literal exists in either binary. The descriptor is data-driven through `KTGL_SHLIB_SHADOWMAP_TYPE` / `_FORMAT`.

### What one session settles

`DUSK_TARGET_CENSUS=1` already logs the right fields — `censusReport` prints dimensions, format, bind flags and caller RVA, and admits anything with `RENDER_TARGET|DEPTH_STENCIL`.

- **Escha**: a `1024x1024 format=44` line with `bindFlags=0x40` means the defect survived 1.01; `0x48` means it was fixed upstream and the item closes. Either way a `1024x1024 format=90 bindFlags=0x28` line should also appear.
- **Shallie**: the same two lines answer whether it has the resource half.
- The planned `samplerCensus()` answers the sampler half for both — watch for `Filter=0x94`. Expected return addresses: Escha `0x3d6d84`/`0x3dec35`, Shallie `0x3b0054`/`0x3b7d65`.

**Run with AGT removed.** Its `CreateSamplerState` patch has **no game gate** — it matches on the descriptor alone — so with AGT installed it is already silently fixing the sampler half in whichever game has it, and no comparison can distinguish the two.

### Do not carry findings between the two KTGL games here

The "byte-identical" pattern that has held everywhere else **breaks on this subsystem**: different terrain shadow shader behaviour, a depth shader present in only one game, and only Escha received a patch.

**Also worth keeping:** AGT's `PSSetShaderResources` branch requires `NumViews == 1` and demonstrably fires, so KTGL sets pixel-shader resources one slot per call — consistent with the per-slot dirty-mask flush already mapped.

## Shallie's control-hint panel

> **TL;DR**: The bottom-**right** controls hint is not driven by an SCL animator — it is a hand-rolled easing loop in one function, `ButtonHelp::Update` (EN `0x48da0`, ML `0x48b80`). Suppressing the slide is a one-function hook that passes a larger `dt`; nothing is patched and nothing is forced into a state the object could not reach itself. Escha does not have this panel at all. **Shipped and validated in game.**

### The animator lead was wrong, and cleanly so

`ktgl::scl::CPaneGroupArrayAnimator` is **not involved**. `rtti` finds four `scl` TypeDescriptors, all `CFunctionCurve::US_KEY_DATA` container templates; `rtti … Pane` and `rtti … Animator` return nothing, so there is no vtable to walk. The names `CPaneGroup`, `CAnimatorBase`, `CPaneAnimator`, `CPaneGroupArrayAnimator` do exist as a contiguous ASCII pool at `.rdata` `0xb7afeb`–`0xb7b0d0`, but a whole-file search for both the 8-byte VA and the 4-byte RVA of `CPaneGroupArrayAnimator` (`0xb7b09d`) returns **zero hits**, and `xref-data` finds no RIP-relative reference. It is unreferenced metadata sitting amid `std::_Compressed_pair<…>` type names.

The SCL classes *are* linked in and used, but through leaf accessors with no RTTI and **no `.pdata` entry** — `0x27b650` `CPane::SetVisible`, `0x27b680` `CPane::IsVisible`, `0x27b330` `GetNthChild`, plus the input-prompt lookups at `0x5511e0`/`0x551240`/`0x54f580`/`0x54f5e0`. A `.pdata`-keyed sweep cannot see any of them. Third time that blind spot has mattered.

### What the panel is

Shallie's UI is data-driven XML shipping loose on disk under `Saves/ui/**` (331 files, with `uil.dtd`/`uia.dtd`). Exactly one referenced string is a controls hint: `Saves/ui/button_help/uil_shade.xml`, referenced at `0x48d6b`. That layout is only the translucent backdrop; the prompts are **20 text panes created in code**.

One translation unit, `0x48470`–`0x4ab7d`:

| RVA (EN) | RVA (ML) | role |
|---|---|---|
| `0x48b40` | — | ctor: stores `this` in global `0xded5a0`; creates **20** panes at `(1280, 678)`, font 18.0, all `SetVisible(false)` |
| `0x48d50` | `0x48b30` | sets the shade layout path, `SetVisible(root, true)` |
| **`0x48da0`** | **`0x48b80`** | **`Update(this, float dt)` — the entire animation.** Called from UI-manager `Update` `0x1c62d0` |
| `0x497f0` → `0x49940` | — | build the 20 prompt strings and a joined comparison string |
| `0x49550` | `0x49330` | `Draw()`; guards on `[this+0xd38]`, submits both `CLayout`s |
| `0x4ac20` → `0x48c80` | — | deleting dtor; clears the singleton |

`homolog` EN→ML: `0x48da0 → 0x48b80` **MATCH**, identical prologue and size `0x7aa`; `0x49550 → 0x49330` **MATCH**, size `0x3d`. Only one instance exists — the vtable VA appears nowhere else.

Content is a static key legend: `0x49940` fills 20 slots with `<IMxx>` icon markup plus the bound key name, from the prompt-kind global `0xddb5e0`, the key-name tables `0x10d0bd0`/`0x10d0c50`, and the `[Pad] AB_SET` swap flag `0xddb0f0`. **The output depends only on prompt kind and key bindings — no game state at all.**

### The animation

```
0x48df9  xmm11 = 1280.0f                ; park position / right screen edge
0x48e06  xmm10 = 0.1f
0x48e0f  minss xmm10, xmm1              ; xmm10 = min(0.1, dt)  <-- the ONLY use of dt
0x48f80  xmm6  = 1240.0f                ; layout cursor
0x48fa5  xmm7  = -8.0f                  ; inter-entry gap
```

Targets lay out **right to left** from 1240. Per slot: non-empty panes ease `x` toward target by `|target - x| * 10.0f * xmm10` with overshoot guards; empty panes ease toward 1280 at `xmm10 * 4000.0f`, then hide on arrival. If the joined string changed this frame, the move is skipped for one frame. Then the shade node is positioned 50 px left of the leftmost visible pane and stretched to the right edge.

So the "entrance animation" is panes parked off-screen sliding left into place while the backdrop grows leftwards. There is no keyframed animation and no animator object to disable.

### Re-animation, not recreation — but the trigger is unresolved

The 20 panes are created once and only ever have their text, visibility and `x` rewritten, so within one object lifetime this is re-animation. Since `0x49940`'s output is a pure function of prompt kind and key bindings, **the slot set cannot change on a dialogue advance**, which means the observed retrigger is probably a lifetime event.

Most likely the containing UI manager is reconstructed per game mode or event step — `0x48b40` is called from `0x1cf0a0`, a vtable slot whose siblings include a heavy init at `0x1cf480`. Every reconstruction re-parks all 20 panes and replays the full slide, which fits "and frequently otherwise".

**Unresolved:** the `[this+0xd39]` suppression flag. `Update` reads it at `0x491dc` and both `Update` and the ctor clear it, but a byte scan for the displacement finds **no writer**. Either it is dead or it is written through a hoisted `lea` — the `writes-to-offset` blind spot — so it is not being called dead.

A three-value per-frame probe settles it: the singleton pointer `0xded5a0`, `[obj+0xd39]`, and pane 0's `x`. A changing pointer means recreation; a toggling flag means the suppression path; neither means re-flow.

### The position discrepancy, resolved

The report said lower-**left**; everything in the binary said bottom-**right**, and **the binary was right** — confirmed in game. Recording the reasoning because it is the kind of disagreement that is cheap to resolve and expensive to guess at: everything in the binary said bottom-**right**: panes park at `x = 1280` (the right edge of the 1280×720 authoring canvas), lay out leftwards from 1240, sit at `y = 678`, and the shade stretches to 1280. The 9-slice art `gen_common03_helpbg` has `edge_margin="271,16,96,61"` — a wide decorated *left* cap and a plain right end that runs off screen, which fits a right-anchored bar whose left end is the part that moves.

The alternatives were ruled out rather than assumed. `Saves/ui/common/uil_line_help.xml` has genuine SCL `anim_ctrl` fade animations and would have been a perfect fit for "replays its entrance animation" — but **its name does not appear in the executable at all**, nor does `uil_prompt`. The shipped `Saves/ui` tree includes the whole authoring set, samples included, so unreferenced layouts are expected. A scan of `.rdata` for bottom-left park coordinates found one unrelated hit.

**Discriminator, now resolved:** the panel slides in horizontally, so this was the right element. Both the position and the mechanism check out.

### Suppression options, ranked

**1 — Force the ease to converge in one frame. SHIPPED, and validated in game.** Hook `Update` and call the original with `dt = 0.1f`. `dt` is consumed at exactly one instruction, and the step is `|delta| * 10.0f * xmm10`; at `xmm10 = 0.1` that is `|delta| * 1.0`, so each pane lands exactly on target in one frame using the game's own overshoot guards. No patched bytes, no unreachable state, switchable per frame. The `0.1` also reaches `CLayout::Update`, which clamps internally to `1/60` — and neither layout carries any `anim_ctrl`, so that is inert. Residual: the *exit* runs at 4000 px/s, so departing prompts still take ~3 frames; raise the forced `dt` if that reads as motion, since the clamp makes it free.

**2 — Hide the panel outright.** No-op `Draw` (EN `0x49550`, ML `0x49330`). `Update` keeps running, so all internal state stays coherent and only display-list submission is skipped. `0x49550` has a second caller at `0x7ab05` on the same global object, which the no-op also covers. This is the bigger behavioural change and should be the option rather than the default.

**3 — Inline patch of the ease block.** Cheapest at runtime but needs an anchor inside a `0x7aa`-byte function; more fragile for no real gain.

**4 — Skip `Update` entirely. Do not.** Two concrete failures: the shade node stays visible, because the XML declares `visible="true"` and only `Update` ever hides it; and `[this+0xd38]`, set by `Draw` and cleared only by `Update`, sticks at 1 and turns `Draw` into a permanent no-op after the first frame.

Do **not** patch the shared float `0x6e2300` (`10.0f`) — it is a generic `.rdata` constant, almost certainly shared.

### Escha does not have this

The byte-identical pattern breaks completely. Escha has **no `Saves/ui` directory at all** — its layout tree is absent, the executable contains one stray `Saves/ui/...` string and no `button_help`, and its UI comes from the older `Data/WinXls` pipeline (`WinFrameNode`, `WinCenterNode`, `UIDataBase::load`). Its `<IMxx>` strings are **pre-composed localized help lines** in the message data rather than Shallie's per-slot tables. `homolog` Shallie→Escha returns MISMATCH for `0x48da0`, `0x48b40` and `0x497f0`; the one "CONFIRMS" vote is spurious (that Escha function references "Gatherable Items" and has a different prologue and size).

One address pack with two rows covers Shallie EN and ML only. Escha needs its own investigation and may not have the behaviour at all — worth one look in game before spending RE time.

### Reference

Globals: `0xded5a0` singleton, `0xddb5e0` prompt kind, `0xddb0f0` AB_SET, `0x10d0bd0`/`0x10d0c50` key-name tables.
Fields: `+0x08` entries `CLayout`, `+0x20` its root, `+0x690` shade `CLayout`, `+0x6a8` shade root, `+0xd18` last joined help text, `+0xd38` drawn-this-frame guard, `+0xd39` suppress flag (reader only).
Constants: `0x6e37c8` = 1280.0, `0x6e37c4` = 1240.0, `0x6e37d0` = -8.0, `0x6e37d4` = -50.0, `0x6e37cc` = 4000.0, `0x6e2300` = 10.0, `0x6e2ad0` = 0.1, `0x6e37e0` = (1280, 678, 0, 0), `0x6e37c0` = 18.0.
SCL helpers: `0x27b830` `FindNode`, `0x27b330` `GetNthChild`, `0x27b650`/`0x27b680` `SetVisible`/`IsVisible`, `0x284560` `SetText`, `0x276360` `SetWidth`, `0x273690` `CLayout::Update`, `0x273750` `CLayout::Draw`, `0x273b30` `CLayout::SetPath`, `0x1fa490` measure text width.

## Battle cut-in shadows

> **TL;DR**: The Arland shadow-reception gate exists in all three Dusk games — verbatim in Ayesha (with the original HLSL still embedded in the shaders), and as a differently-written linear ramp with the same `0.7` breakpoint in Escha & Logy and Shallie. One animated constant produces both symptoms in both engines. Not a defect; an enhancement, so it ships off behind an ini key and a launcher control when it ships at all.

### Ayesha: a direct homolog, with the source still in the binary

Ayesha's `Res/x64/cg/commonShader.PSSG` DXBC containers carry **`SPDB` chunks containing the full preprocessed HLSL**, original paths included (`F:\DLeft\Left\A14DX\application\src\cg_x64\fieldmap\...`). The gate is therefore readable as source rather than reconstructed:

```hlsl
VrtOutShadow_s convVrtShadow(AppOut_s IN) {
    if( dot(MN, L) <= 0 ){
        OUT.ShadowAlpha = 2.0f;
    }else{
        OUT.ShadowAlpha = 2.5f - min(diffuse[0], diffuse[3])*2;
        OUT.ShadowAlpha += max((Distance-20.0f), 0.0f) / 40.0f;
    }
...
float4 convFrgShadow(float4 i_f4Color, VrtOutShadow_s IN) {
    if(IN.ShadowAlpha >= 1.0f) { oColor = i_f4Color; }   // no shadow sample at all
    else { float shadow = calculateShadow2(IN.oShadowUV, tapScale, PSSGShadowMapDepthMap); ... }
```

Reception is gated shut when `min(diffuse.x, diffuse.w) <= 0.75` — the identical threshold to Rorona and Meruru. **One term Arland's write-up does not mention**: a distance falloff that also closes the gate beyond roughly 40 eye-space units even at full intensity (`ShadowAlpha = 0.5` at `diffuse = 1.0`, then `+= max(D-20,0)/40`).

Confirmed at bytecode level, not only in the embedded source. Vertex shader `fieldmap_shadow_normal_vert` (container 46):

```
1144 min  temp[0].y, cb[0][45].w, cb[0][45].x
1153 mad  temp[0].y, temp[0].y, (2), (2.5)      ; NEG modifier -> 2.5 - 2*min
1186 movc output[1].w, temp[0].x, (2), temp[0].y
```

and in every receiver pixel shader (containers 44, 45, 47, 48, 49, 50):

```
  71 ge   temp[1].x, input[1].w, (1)
  78 if_z temp[1].x
  88   <gather4_c on PSSGShadowMapDepthMap / shadowMapSamp>
 220 endif
```

`fieldmap_shadow_*` is the **only** shadow-receiving family in Ayesha: of 139 DXBC containers, exactly six bind `PSSGShadowMapDepthMap`, and all six are its pixel shaders. Characters (`chara_mt*`, `toon_p0*`) cast but never receive — exactly as in Arland.

#### Constant-buffer layout

| family | VS `$Globals` | PS `$Globals` | `tapScale` | **`diffuse`** | `shadowLPos` |
|---|---|---|---|---|---|
| `fieldmap_shadow_normal` (44, 45, 46) | 752 | 768 | +704 | **+720** (`cb0[45]`) | +736 |
| `fieldmap_shadow_fog[_layoff][_op]` (47–52) | 784 / 800 | 800 / 816 | +736 | **+752** (`cb0[47]`) | +768 |

The 16-byte `$Params` `diffuse` (cb1) that Arland's dim-hold targets is present on containers 44, 45, 47 and 49; 48 and 50 fold it into `$Globals` instead. `tapScale` sits at the same relative position, so the PCF rescale has an equivalent here too.

#### The battle arenas are these receivers

All 33 `Res/x64/btlField/BF_POINT*.PSSG.gz` assets contain meshes tagged `xSHADOWON` (`ground_xSHADOWON_B`, `LAYER_030SHADOW_001`) — the same naming the field maps use (`gr_xSHADOWON009` in `fieldmap/map/EVENT_01.PSSG.gz`). The nine DXBC shaders embedded in `BF_POINT01.PSSG.gz` bind no shadow map at all, so the receiving variant comes from the engine's `commonShader.PSSG`. **INFERRED** from asset naming plus "only family that can receive" — and it is the same conclusion Totori reached at runtime.

#### The caster cover-up is present and byte-identical

| | Rorona EN | Ayesha EN | Ayesha ML |
|---|---|---|---|
| tactical `hideAll` | `0x10c2c0` | `0x16c8c0` | `0x171450` |
| tactical `showAll` | `0x10c270` | `0x16c860` | `0x1713f0` |
| deferred-hide arm leaf | `0xc5f80` | `0xfa6b0` | `0xfee30` |

`homolog` returns MATCH with raw-identical prologues for `hideAll` and `showAll`; Ayesha's `showAll` matches Arland's *Rorona* prologue variant, not the Meruru/Totori one. The thirty-byte leaf is byte-for-byte Rorona's, **including the Model offsets**:

```
0x000fa6b0  38 91 80 00 00 00 74 15 f3 0f 11 91 90 00 00 00
0x000fa6c0  c6 81 8f 00 00 00 01 88 91 8e 00 00 00 c3
```

so visibility byte `+0x80`, fade-pending `+0x8f`, duration float `+0x90`, plus the `+0x8e` write. **Ayesha uses the Rorona/Meruru Model layout, not Totori's.** The `hideAll` caller `0x143390` (through thunk `0x188a4`) passes the same quarter-second fade, loading `0x9b75e8` = `0.25f` into `xmm1`. So all three windows exist with the same timing; budget for the same three-window front-run rather than expecting to skip it.

Ayesha's RTTI also carries the full `GmStateBtl*` family (26 vtables), `BtlChara`/`BtlCharaMgr`/`BtlCharaParty`/`BtlCharaMonster`, `BtlField`/`BtlFieldMap` and `PSSG::PNode`, so the cinematic-state list and BtlChara-vtable tracking port directly.

**Unresolved: what animates `diffuse` down.** Not found statically — but **Arland never found it either.** `battle_shadows.cpp` identifies the dim *by value shape* at draw time (`(s,s,s,~1)` with `s` in `(0.5, 0.98)`) rather than by address, and that approach transfers unchanged; only the per-size field table needs the Ayesha rows.

### Escha & Logy and Shallie: same idea, different formulation

KTGL shaders live in `Data/x64/Shader/*.g1s` — KTGL's own container with DXBC inside and **no `SPDB`**, so bytecode only. The two games' shader sets are equivalent for this purpose.

Only `MapSTD*` (plus `STD-B*` and `MapWater`) pixel shaders bind `sShadow`; `Chara*` never receive. The `MapSTD*` PS `$Globals` is 80 bytes and identical in both games:

```
+0   nStageNum    cb0[0].x
+4   vATest       cb0[0].y
+16  scSM         cb0[1]
+32  scSM2        cb0[2]
+48  atColor      cb0[3]      <-- the gate constant
+64  HdrRangeInv  cb0[4].x
```

Shadow sampling here is **unconditional** — no `if` around the 64-tap PCF, unlike Ayesha. Instead the accumulated *lit* fraction is scaled by a ramp on `atColor.x` immediately after the taps:

```
2842 add  temp[2].y, cb[0][3].x, (-0.7)
2850 mul  temp[2].xy, temp[2].xyxx, (0.0625, 3.33333, 0, 0)
2860 max  temp[2].y, temp[2].y, (0)
2867 mul  temp[2].x, temp[2].y, temp[2].x        ; s = max(atColor.x-0.7,0)/0.3 * (taps/16)
2881 mad  temp[2].yzw, temp[2].xxxx, temp[2].yyzw, input[1].xxyz   ; diffuse  = s*direct + ambient
2897 mad  temp[0].xyz, temp[2].xxxx, temp[0].xyzx, input[8].xyzx   ; specular = s*direct + ambient
```

and `atColor` darkens the final colour directly as well:

```
2956 add  temp[0].xyzw, cb[0][3].xyzw, (-1,-1,-1,-1)
2967 mov  temp[1].xyzw, cb[0][3].xyzw
2973 mad  temp[0].xyzw, temp[3].xyzw, temp[1].xyzw, temp[0].xyzw   ; out = c*atColor + (atColor-1)
```

**One animated constant, both symptoms**, exactly as in Arland. At `atColor.x = 1.0` the ramp is 1 and the combine is identity; as it falls to `0.7` the direct-light term — which is what carries the shadow contrast — is scaled to **exactly zero** while the floor darkens. `0.7` is literally the value Arland's cut-in animates to.

Verified in every shadow-receiving `MapSTD*` in both games, with the identical `-0.7` / `3.33333` pair and `atColor` at +48 throughout: Escha `MapSTD.g1s` idx 36, `MapSTD-B0`/`B0T0`/`B1`/`B2`/`-T0` idx 12, `MapSTDHS` idx 24, `MapSTDNF` idx 2, `MapSTDNS` idx 24; Shallie the same at idx 42 / 14 / 28 / 2 / 28, plus `MapSTDS.g1s` idx 14. `STD-B0/B1/B2/-T0` (props, not ground) bind `sShadow` but have **no** `atColor` and no ramp — the same "only the ground is gated" structure as Arland and Ayesha.

Escha's battle arenas live in the *same* `Data/x64/Field/` tree as ordinary areas (`BF_POINT01`…`BF_POINT32` alongside `AREA_*`). Decompressing `Field/BF_POINT01/BF_POINT01.elixir.gz` yields the material parameter names `sLit sShadow scSM scSM2 smpsLit smpsShadow` — the exact `MapSTD` receiver binding set, identical to `AREA_01_01`. **VERIFIED for Escha**; Shallie's `.elixir.gz` uses a `CRAE` container that was not decoded, so **INFERRED for Shallie** from the identical shader set.

`atColor` is a genuinely animated float4, not a constant: it is a named shader parameter (Escha `.rdata` `0x95aaf0`, Shallie `0x96c468`) bound by a setter at Escha `0x505940`. Several call sites push neutral `(1,1,1,1)` / `(1,1,1,0)` literals, and one — `0x286f40`, called from `0x28bec0` — builds the value as a **keyframe lerp of a float4** from `[rbx+0x38..0x44]` toward `[rbx+0x48..0x54]`. A uniform `(0.7,0.7,0.7,1)` is exactly what that path can produce.

**Unresolved:** that a battle cut-in is what drives `atColor` to <= 0.7. That is the one runtime question, and it is cheap — a draw-time trace of the 80-byte `MapSTD` PS cb0 during a cut-in, reading dwords `[48,64)`.

### Implementation notes

- Ayesha needs a per-`ByteWidth` field table (`752`/`784`/`800` VS, `768`/`800`/`816` PS) with `diffuse` at offset 720 or 752 — structurally Arland's Totori table, not the single classic entry.
- Escha and Shallie need one entry: `ByteWidth == 80`, offset 48, hold target `1.0` for `.x`. Holding all four components at 1.0 also removes the direct darkening.
- Arland's value predicate transfers to Ayesha unchanged. For KTGL it must be widened or replaced: `atColor` is legitimately `(1,1,1,alpha)` on some object paths, so the predicate wants to be `.w`-insensitive and test `.xyz` only.
- **The KTGL caster cover-up is a fresh hunt.** The Arland/Ayesha `hideAll`/`showAll`/arm-leaf are PhyreEngine functions and those bytes are not in a KTGL binary. The game-side callers are shared — both KTGL executables carry the full `GmStateBtl*` (Escha 27 vtables, Shallie 25) and `BtlChara*`/`GameModeBattle`/`BtlCamera` sets — so the entry point is the KTGL analogue of Ayesha's `0x143390`. But the KTGL gate is a **linear ramp rather than a hard threshold**, so there is no crisp closed-gate window concealing stale casters; the exposure may be partial rather than binary. Establish that before designing anything.
- Both playbook §5 traps fired again during this pass, exactly as documented: `funcof ayesha-en 0xfa6b0` errors because the leaf has no `.pdata` entry (found instead with `find-bytes ayesha-en "74 15 f3 0f 11 91"`, one hit), and `callsites` on `hideAll`/`showAll` returned nothing usable until re-run against the thunks `0x188a4` and `0x22052`.

## The resource-lock middleware

> **TL;DR**: All six DX ports share one Gust D3D11 resource-lock middleware — the thing the whole `atelier-sync-fix` lineage exists to fix. It is present in all three Dusk games, identically. But Ayesha's text path locks in the two modes no implementation of the fix can reach, and the atlas cache has already emptied it. The open question is Escha & Logy, Shallie, and Ayesha's five non-atlas locks, and it needs one instrumented run.

### What the sync fix actually addresses

Read out of `atelier-arland-fixes/src/sync_fix.cpp` and Rorona's own code rather than from prose. The stall comes from a middleware whose lock has exactly this shape:

```
CreateBuffer/CreateTexture2D/CreateTexture3D(Usage = STAGING, BindFlags = 0,
                                             CPUAccessFlags = table1[mode])   // fresh, per lock
CopyResource / CopySubresourceRegion(newStaging <- realGpuResource)           // GPU work
Map(newStaging, 0, table2[mode], 0, &mapped)                                  // CPU waits for the GPU
```

Two 4-entry mode tables select the behaviour, byte-identical in every binary:

| mode | `CPUAccessFlags` | `D3D11_MAP` | copies first? |
|---|---|---|---|
| 0 | `0x20000` READ | 1 `READ` | yes |
| 1 | `0x10000` WRITE | 2 `WRITE` | yes |
| 2 | `0x30000` READ\|WRITE | 3 `READ_WRITE` | yes |
| 3 | `0x10000` WRITE | 2 `WRITE` | **no** — a `DYNAMIC` resource short-circuits to a direct `Map(WRITE_DISCARD)` |

Table RVAs: Rorona EN `0xa4d758`/`0xa4d768`, Ayesha EN `0xfe85c0`/`0xfe85d0`, Escha EN `0x950960`/`0x950970`, Shallie EN `0x946200`/`0x946210`.

The fix replaces the GPU copy with a CPU `memcpy` out of a *persistent* shadow staging resource attached to the source via `SetPrivateDataInterface`. Everything else in it — the `Map`/`Unmap` redirect, dirty coalescing, flush points — exists to keep that shadow coherent, because the game writes the same resources through `Map`.

### The mode argument is the whole story

`tryCpuCopy` gates on `isCpuWritableResource(dst)`: `STAGING|DYNAMIC` **and `CPU_ACCESS_WRITE`**. Mode 0 creates its staging with `CPU_ACCESS_READ` only, so the gate fails and the copy falls through to the real GPU path — in **upstream's, TellowKrinkle's and Arland's implementations alike**. Mode 3 issues no copy to intercept.

Ayesha's atlas census records exactly two callers: `renderText` at **mode 0**, and the write-begin helper at **mode 3**. Arland's measured atlas readbacks are `cpu=0x30000`, i.e. **mode 2**, which *is* eligible. **That is why the fix pays in Arland and cannot pay on Ayesha's text path** — and it is the specific reason the "Ayesha is old-Arland, port the fix" reasoning in `archive/DUSK/LANDSCAPE-2026-07-26.md` does not hold.

The mode is the lock's 4th parameter (`r9d`), which is how `atlas_fix.cpp`'s `atlasLock(texture, output, level, mode)` already reads it.

The only way to make mode 0 eligible would be to add `CPU_ACCESS_WRITE` to the staging descriptor inside the mod's own `CreateTexture2D`/`CreateBuffer` hook. Legal and behaviour-preserving from the game's side, but a new mechanism rather than a port, and it changes driver-side memory placement. Not worth it for a path the atlas cache has already emptied — 95.5% of those locks never reach D3D11, leaving ~3 real read locks per frame.

### Six locks per binary, and the same revision across the trilogy

Rorona's texture lock `0x3ee6e0` homologs into every Dusk binary with byte-identical prologues and forward-plus-reverse confirming votes. Searching each binary for references to the two mode tables enumerates the complete set: **six lock functions in every English build**.

| # | kind | Rorona EN | Ayesha EN | Escha EN | Shallie EN |
|---|---|---|---|---|---|
| 1 | buffer | `0x3eb800` | `0x57d650` | `0x3eeff0` | `0x3c6410` |
| 2 | buffer | `0x3ebe90` | `0x57df10` | `0x3d0010` | `0x3a2500` |
| 3 | **Texture2D — the atlas lock** | `0x3ee6e0` | `0x580f90` | `0x3f1a30` | `0x3c8b50` |
| 4 | Texture2D, second variant (no fast path) | `0x3eee40` | `0x581950` | `0x3f2200` | `0x3c92f0` |
| 5 | Texture3D (no fast path) | `0x3ef450` | `0x582150` | `0x3f2870` | `0x3c9950` |
| 6 | buffer | `0x3effe0` | `0x5830d0` | `0x3f34c0` | `0x3ca570` |

Ayesha, Escha and Shallie have byte-for-byte identical extents for all six (texture lock `0x386`, buffer lock 1 `0x30b`); Rorona's are slightly smaller (`0x312`, `0x293`). The three Dusk games ship one and the same, marginally newer, revision of the middleware.

Ayesha ML texture lock is `0x5a3490` — the address this document already names as "the atlas lock's own implementation". So the function the menu fix hooks **is** the middleware texture lock, and the atlas cache and a sync fix would be two layers on the same call.

Per-lock D3D11 call sites, for keying a probe (create / copy / map, with the fast-path `Map` in parentheses):

| lock | Ayesha EN | Escha EN | Shallie EN |
|---|---|---|---|
| buffer 1 | `0x57d831`/`0x57d895`/`0x57d8ce` (`0x57d766`) | `0x3ef1d1`/`0x3ef235`/`0x3ef26e` (`0x3ef106`) | `0x3c65f1`/`0x3c6655`/`0x3c668e` (`0x3c6526`) |
| buffer 2 | `0x57e0d5`/`0x57e139`/`0x57e172` (`0x57e003`) | `0x3d01d5`/`0x3d0239`/`0x3d0272` (`0x3d0103`) | `0x3a26c5`/`0x3a2729`/`0x3a2762` (`0x3a25f3`) |
| tex2D (atlas) | `0x5811b9`/`0x58123f`/`0x581275` (`0x581073`) | `0x3f1c59`/`0x3f1cdf`/`0x3f1d15` (`0x3f1b13`) | `0x3c8d79`/`0x3c8dff`/`0x3c8e35` (`0x3c8c33`) |
| tex2D #2 | `0x581a8e`/`0x581b14`/`0x581b49` | `0x3f233e`/`0x3f23c4`/`0x3f23f9` | `0x3c942e`/`0x3c94b4`/`0x3c94e9` |
| tex3D | `0x58229c`/`0x58231d`/`0x582351` | `0x3f29bc`/`0x3f2a3d`/`0x3f2a71` | `0x3c9a9c`/`0x3c9b1d`/`0x3c9b51` |
| buffer 3 | `0x5832d0`/`0x583338`/`0x583371` (`0x583213`) | `0x3f36c0`/`0x3f3728`/`0x3f3761` (`0x3f3603`) | `0x3ca770`/`0x3ca7d8`/`0x3ca811` (`0x3ca6b3`) |

### The engine split does not apply to this item

The triage above stands for the renderers — Ayesha's text renderer is an exact Arland homologue and Escha/Shallie have no n-gram matches for it. But **the D3D11 resource layer underneath is the same code in all six games.** Escha's texture-lock stub `0x3f1dd0` has 6 direct callers and its buffer-lock-1 stub `0x3eefc0` has 4, one of which (`0x66f990`) sits far outside the middleware address range — KTGL-level engine code reaching the middleware buffer lock. Escha and Shallie additionally carry ~9 `CopyResource` sites that Ayesha and Rorona do not, identical between the two of them.

So the correct framing is: the mechanism is common, only the call frequency and mode mix are per-game.

### What the existing probe already showed, and what it could not

The `D3D11PROBE` run recorded here — 4,908 writes to 512×512 textures from two call sites, `0x5a3576` and `0x5a3746` — resolves against the ML lock at `0x5a3490` as:

- `0x5a3576` = the **fast-path direct `Map(WRITE_DISCARD)`** on the dynamic atlas (EN `0x581073`);
- `0x5a3746` = the return of the **`CopySubresourceRegion(staging <- atlas)`** of the staging round trip (EN `0x58123f`).

Two things follow. The staging round trip is confirmed firing at runtime, on the **immediate** context, despite Ayesha recording its frame on a deferred one — which settles the deferred-context worry, since `Map(READ)` on a deferred context is illegal and the middleware uses the one context in its own global. And the probe's filter reports **write** map types only, so `Map(READ)` and `Map(READ_WRITE)` — the stalling ones — were never in scope. **The existing evidence is silent on exactly the population that matters.**

### The measurement

`src/core/d3d11_probe.cpp` is the right host; three additive changes.

1. **Invert the map filter.** It early-outs on anything that is not a write map and on anything that is not 512×512. Want the opposite: every `Map` with `READ` or `READ_WRITE`, any resource shape.
2. **Time the real call.** Wrap `originalMap` in a `steady_clock` pair — the count is not the answer, the stall microseconds are. Arland's `ReadMapKey`/`ReadMapStats` and its per-interval emitter port essentially unchanged, keyed on `(callerRva, dim, format, WxH, usage, bindFlags, cpuFlags)`. **`cpuFlags` in that key is what reveals the lock mode, and therefore eligibility, with no further RE.**
3. **Count staging creations** on device slots 3/5/6 with `Usage == STAGING`. Arland measured 1,695 creations in one Rorona transition at 1.60 ms each, so creation churn may be the larger cost.

Fixed resolution, vsync and frame cap off, three fixed scenes per game: title/main menu, a fixed field spot standing still, battle first turn. Run Ayesha's menu scene twice, `DUSK_ATLAS_CACHE=0` and `=1`, or the cache masks the credit.

**Decision rule.** Build nothing unless a scene shows a material per-frame total in `api_us` attributable to `READ`/`READ_WRITE` maps whose `cpuFlags` include `0x10000`. If the microseconds are there but `cpuFlags` is `0x20000` everywhere, the answer is "the pattern exists and no port can reach it" — record that and close the item.

**Trap.** `d3d11InstallHooks` runs before `initializeD3D11WriteProbe` (`main.cpp`) and both want context slots 46/47. With MSAA enabled MinHook returns `MH_ERROR_ALREADY_CREATED` and the probe logs "installing nothing". Measure with MSAA off, or move the counters into the existing detours.

### If a fix is warranted, only Arland's is viable

| | covers mode 1/2 | `Map`/`Unmap` coherence | cost per `Unmap` | deferred-context safe |
|---|---|---|---|---|
| upstream (doitsujin v0.5) | yes | **no** — shadows refresh only on RTV/UAV bind | – | no `ExecuteCommandList` boundary |
| TellowKrinkle `98b5c9b` | yes | one **global** last-mapping slot; uploads the **whole** resource every `Unmap` | high | not explicitly |
| **Arland per-resource** | yes | keyed `(resource, subresource)`, COM refs held for the mapping's life, `READ_WRITE` redirect preserving unwritten pixels, dirty-marking coalesced before draw/dispatch/GPU-copy/`ExecuteCommandList`, immediate-context only | one coalesced upload | yes, by construction |

Upstream's is not merely worse, it is *incorrect here*: all three Dusk games write these resources through `Map`, which is precisely the corruption TellowKrinkle's commit was written for. Ayesha's deferred-context frame recording makes the `ExecuteCommandList` flush boundary load-bearing, and Arland already has it.

Two hazards not to port verbatim:

- Arland's sync fix asserts that immediate and deferred contexts share one vtable and copies the immediate proc table into the deferred one. **This is measured false on Ayesha** — see "Why it did not fire: the wrong context vtable" above, two distinct vtables (`…4410` / `…3ca0`). A port inheriting that assumption will silently mis-dispatch.
- `isMutableFontAtlas` (dynamic 512×512 → refuse to shortcut) guards against snapshotting an atlas the deferred queue filled before the shadow existed. Its shape assumption is validated for Ayesha and **unvalidated for Escha and Shallie**.

**Feature collision, specific to this repo.** MSAA already owns slots 46/47 and resolves its multisample twin into the host *before* the copy, so a CPU-copy shortcut reading the host directly would read an unresolved surface. Ordering between the two must be explicit; Arland gets it for free by living in one translation unit and this project would not. The same applies to the high-res target substitution and the SSAA back-buffer redirect.

### Risk, and what would falsify a claim that it works

Corruption looks like: garbled or blank glyphs and missing kanji from a stale shadow (the exact Arland bug the invalidation rule was written for); on buffers, geometry from a previous frame, flickering meshes, wrong bone transforms. A shadow snapshotted before the deferred queue filled the source produces content that is *consistently* wrong and never self-corrects — that signature is what distinguishes it from a race.

Lifetime: the shadow hangs off the source via `SetPrivateDataInterface` and dies with it. The failure modes are a missed `Release` on the internal `Map` paths (leak, then OOM on a long session) and mapping a shadow whose source died (crash on scene change). Memory grows by one staging copy per distinct source resource — bounded in Arland because the destinations are throwaway, unmeasured for Escha and Shallie.

A claim that it works must survive all four:

1. the census counters for CPU copies and shadow flushes are **non-zero** in the scene where the win was measured — if they are zero, the delta is not the fix;
2. the frame-time win reproduces on a fixed scene across at least three runs;
3. a pixel hash of a fixed frame is identical with the fix on and off;
4. text is byte-identical across a language switch and a kanji-heavy string, and a long session shows no RSS growth.

Passing (2) while failing (1) is the classic false positive here, and this project's history has several.

## Controller-absence rescan

> **TL;DR**: All six DX ports share one Gust input layer that rescans for a pad once per 60 input-update ticks while none is present, and stops permanently once one is found. The shape matches the reported stutter. The suspected cost is DirectInput enumeration under Proton, which is **not measured**. No code yet.

### The hypothesis that turned out to be wrong

The reported symptom — repeated stutter with no controller connected — has a classic cause: polling all four XInput slots every frame with no backoff, since `XInputGetState` on an empty slot is historically expensive. **These games do not do that**, and the standard remedy for it would not help here.

- Every executable imports exactly **two** XInput entries, **by ordinal**, from `XINPUT1_3.dll`: ord 2 `XInputGetState`, ord 3 `XInputSetState`. Ordinals confirmed against the `xinput1_3.dll` in the game prefixes. No `XInputGetCapabilities`, no `XInputEnable`, no `RegisterRawInputDevices`, no `HidD_*`, no `joyGetPosEx`.
- `XInputGetState` has exactly **two** static call sites per executable: the pad create and the pad update.
- The `0..3` loop in the create function is a **free-slot search over an internal occupancy bitmask**, not an XInput enumeration. It exits at the first clear bit and issues exactly one `XInputGetState` — always slot 0 when nothing is allocated.
- The pad update path is reachable only for a pad that was created over XInput, so with no controller it never runs at all.
- **A backoff already exists.** `cmp eax, 0x3c` gates the rescan to once per 60 input-update ticks, and a non-null `g_pad0` skips the block outright.

The engine split does not apply here: Ayesha's pad-create is byte-identical to Escha's and Rorona's modulo displacements. This is Gust's own pad layer, shared across all six ports, PhyreEngine and KTGL alike.

### Where the two engines do diverge, and why it decides the fix

- **Ayesha and the three Arland games** run the descriptor **twice** per rescan. `test ebx,ebx / cmove r8, rsi` forces the `IDirectInput8*` field to NULL on pass 0, taking the XInput branch, then restores it on pass 1 for the DirectInput branch. Cost per rescan: one `XInputGetState(0)` **plus** one `IDirectInput8::EnumDevices(DI8DEVCLASS_GAMECTRL, cb, ref, DIEDFL_ATTACHEDONLY)`.
- **Escha & Logy and Shallie** make a single attempt and always pass the live `IDirectInput8*`, so `PadCreate`'s `cmp [rcx+8], 0` never takes the XInput branch. **Their XInput code is dead unless `DirectInput8Create` itself failed**, and their entire controller-absence cost is DirectInput enumeration. They also retry a keyboard device and a third device in the same block.

So hooking `XInputGetState` — the textbook remedy — would do **nothing** for two of the three Dusk games and would remove only the cheap half in the third.

### Addresses (EN builds)

| | Ayesha | Escha | Shallie | Rorona | Totori | Meruru |
|---|---|---|---|---|---|---|
| input update (holds gate) | `0x1a2260` | — | — | `0x13cef0` | `0x1975c0` | `0x15ca00` |
| `cmp eax, 0x3c` gate | `0x1a239f` | `0x4d29b4` | `0x54f834` | `0x13d018` | `0x1976e8` | `0x15cb28` |
| `EnsurePad` | `0x1a2b50` | `0x4d29a0` | `0x54f820` | `0x13d2c0` | `0x197ae0` | `0x15cda0` |
| PadCreate wrapper (CS-guarded) | `0x584fb0` | `0x5d0640` | `0x5d5170` | `0x3f1e60` | `0x4d58a0` | `0x3eb980` |
| PadCreate (DI-or-XInput branch) | `0x585010` | `0x5d0690` | `0x5d51c0` | `0x3f1eb0` | `0x4d58f0` | `0x3eb9d0` |
| XInput pad create | `0x585220` | `0x5d0830` | `0x5d5360` | `0x3f2050` | `0x4d5a90` | `0x3ebb70` |
| XInput pad update | `0x585d60` | `0x5d1140` | `0x5d5c70` | `0x3f2960` | `0x4d63a0` | `0x3ec480` |
| `g_pad0` | `0x166c0e0` | `0x10c3040` | `0x10d0bb0` | `0x10e6b50` | `0xcddc00` | `0xfe74a0` |
| retry counter | `0x166d2f0` | `0x10c3058` | `0x10d0bc8` | `0x10e7768` | `0xcde818` | `0xfe83c0` |

Every wrapper and `EnsurePad` RVA is a confirmed primary function start. Ayesha's chain routes through incremental-link thunks (`0x8e72`, `0x124b3`, `0x1ea5b`, `0x2944c`) and had to be enumerated through them; Escha, Shallie and the Arland games call directly. Escha's `IDirectInput8*` global is `0x10c31d0`, written by the `DirectInput8Create` caller `0x4d3670`.

Ayesha's XInput IAT slots sit at implausible-looking RVAs (`0x50dbbd08`, `0x50dbb128`) — that is real, not a parsing error: its `.data` has `VirtualSize = 0x4fa861f1`, pushing every later section past `0x50000000`.

### What is not established

That `EnumDevices` under Proton is expensive enough to be visible. Wine routes it through the HID/SetupDi device tree, which is plausibly multi-millisecond on a machine with several HID devices — but that is reasoning about Wine, not evidence from these binaries. The XInput half is almost certainly negligible: one call, one slot, once per ~61 ticks.

The cadence is also inferred rather than measured. The gate counts calls to the input-update entry point, not wall time. That entry has 33 static callers in Ayesha, 16 in Rorona, 7 in Escha — the shape of one "poll input" helper called once per iteration of whichever loop is active. If that is once per rendered frame, the period is ~1 s at 60 Hz and ~0.42 s at the 144 Hz this project tests at.

### The measurement that settles it

1. Boot each game with **no** controller, then **with** one, and confirm the stutter appears only in the first case.
2. Put a QPC timer around the PadCreate wrapper and log the duration. A periodic spike ≥ 3 ms confirms the mechanism; consistently < 0.5 ms falsifies it and the stutter is something else entirely.
3. Log the wall-clock interval between successive wrapper entries. That converts the 60-tick gate into an observed period, settles the cadence question, and can be compared directly against the observed stutter period.

### The fix, if it is confirmed

Hook the **PadCreate wrapper** and return NULL without calling through unless N seconds of wall clock have elapsed since the last *failed* attempt. That is precisely scoped to the pad, leaves Escha's and Shallie's keyboard and third-device retries alone, and preserves hot-plug at a longer period. All six targets are primary function starts, so the house `matches(target, *Expected)` + `installMinHookDetour` idiom applies unchanged; the ML builds would need `homolog` runs.

Rejected alternatives: hooking `EnsurePad` also suppresses the other device creations in the KTGL games; patching the `cmp eax, 0x3c` immediate tops out at `0x7f` because it is an imm8; vtable-hooking `IDirectInput8::EnumDevices` (slot 4) reaches the actual cost but needs the live object read out of the global after startup and is the most fragile of the four.

Arland carries the mechanism instruction-for-instruction, so one hook table covers all six games. It has also shipped a long time with no such report, which is weak evidence against the hypothesis.

## The "Loadning system data." typo

> **TL;DR**: Escha & Logy and Shallie misspell "Loading" on the first screen of the game. The word is a plain string literal in each executable's `.rdata`, so the mod corrects the 22 bytes in the loaded image at startup. **Validated in game in both titles** — the KTGL module's first shipped fix. Nothing on disk is touched.

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

### Confirmed in game

**Validated in both games** — the corrected line reads `Loading system data.` on
the first screen of Escha & Logy and of Shallie. This is the KTGL module's first
shipped fix.

One operator note from that validation, worth keeping because it cost a run: the
first Shallie test appeared to fail, and the cause was that only Ayesha had been
redeployed — Shallie was still running a DLL from earlier in the day.
`tools/build-deploy-dusk.sh` fans out to all three games by default, so the fix
is to use it rather than copying by hand. Deploying over a *running* process is
the other half of the same trap: the script renames into place rather than
overwriting the inode, but a game already running keeps the old DLL mapped.

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
