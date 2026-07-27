# Technical overview

The authoritative record of what this mod actually does, and the verified
address packs behind it. Same policy as the Arland project: this file stays in
sync with the code and documents shipped, measured behaviour. Work in progress,
open questions and unvalidated ports are tracked outside this repository and
deliberately do not appear here.

This project is much earlier than its Arland sibling. One fix ships — the Ayesha
font-atlas read cache — plus the diagnostics that justified it. The opt-in
switches listed in the README beyond those are experimental and are not
documented as established behaviour.

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
src/core/    engine-agnostic: the D3D11 proxy, engine dispatch, the capability
             matrix, hook installation, logging
src/phyre/   Ayesha — the atlas read cache
src/ktgl/    Escha & Logy and Shallie — fingerprinting only, so far
```

Neither engine module includes the other's headers, and no address pack lives in
`src/core`. Nothing either module knows is meaningful in the other's process.

They still ship as **one `d3d11.dll`**. Every fix is gated twice, on the
capability matrix in `core/game.cpp` and on an executable fingerprint, so a
module loaded into the wrong process installs nothing; splitting the artifact as
well would buy no safety and cost the user a per-game download.
`core/engine.cpp` resolves the engine from the executable name once and forwards
initialization and the frame tick to that module only.

`src/ktgl/` implements no fix yet, so all it does is establish identity: it
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

`DUSK_FIELD_TRACE=1` wraps the field controller's per-frame update and, on each
ground-contact change, dumps a short ring of frames either side of the event. It
only writes inside those windows, so a character resting quietly produces no
output at all.

## Runtime memory manipulation

> **TL;DR**: The mod does not edit or replace the games' executable files. Its proxy DLL is loaded alongside the game, makes narrowly verified changes inside that running process, and forwards everything else to the real Windows library. Those changes disappear when the process exits.

The 64-bit `d3d11.dll` is a proxy for the system D3D11 library. It exports the
device-creation functions the game expects and forwards them to
`d3d11_proxy.dll` when one is deliberately installed for chain-loading, or to the
real `d3d11.dll` from the Windows system directory otherwise. This lets the mod
observe device creation and install its hooks without replacing the graphics
implementation. There is no 32-bit launcher proxy in this repository.

Executable changes are detours. After verifying the target function, MinHook
places a jump at its entry point and preserves the displaced instructions in a
trampoline, so the mod can do work before or after normal engine behaviour rather
than replacing the routine. MinHook is used rather than this project's own
fixed-size absolute-jump installer because the hooked atlas unlock is a 14-byte
stub, too small to patch without clobbering what follows.

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

Escha & Logy and Shallie are recognized for identification and logging only. They
have no menu hooks and no mapped addresses, and the capability matrix hard-offs
every feature for them so no configuration can turn one on. Their four
executables are fingerprinted the same way, so that a future fix has a verified
identity to gate on:

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
its environment override. The matrix is the source of truth mirrored by the
feature table in the README. There is no ini layer; every switch is an
environment variable.
