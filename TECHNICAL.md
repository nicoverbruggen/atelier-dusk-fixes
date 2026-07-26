# TECHNICAL

Authoritative record of what has been established about the Dusk DX ports and
what this repository does about it. Same policy as the Arland project: this file
stays in sync with the code, and every address pack records how it was derived.

Status: the Ayesha menu hitch is mapped statically (§1), **measured** (§2), and
fixed by the font-atlas read cache in §4, which ships **on by default** — the one
shipping fix in this repository. A second, distinct problem, the main menu
re-rendering its text every frame, is measured and partly mitigated by that cache
but not properly fixed (§2.3, §3). Everything else here is opt-in and unvalidated.

---

## 1. The Arland menu-hitch pattern in the Dusk trilogy

### 1.1 What was being looked for

The Arland project's menu-hitch fix hooks four game-side entry points per
executable — resource-event **queue drain**, **text renderer**, middleware
**atlas lock**, middleware **atlas unlock** — and serves repeated 512×512
font-atlas reads from a CPU snapshot with a queue- or frame-scoped lifetime.
See `../atelier-arland-fixes/TECHNICAL.md` §"Repeated font-atlas reads".

The question for Dusk: do those four entry points exist in the Dusk
executables, and in the same relationship to each other?

### 1.2 Method

Static homologue matching — shared exact byte n-grams voted per `.pdata`
function, verified in both directions, plus prologue comparison — run from all
three Arland English builds into each Dusk executable. This is the same
technique that located the Arland multilingual entry points. Weak verdicts were
corroborated by independent caller-shape comparison rather than accepted on the
vote alone.

### 1.3 Result: Ayesha matches; Escha & Logy and Shallie do not

**Ayesha DX** reproduces the pattern exactly. All three Arland games vote
independently to the same four Ayesha addresses, and the prologues are
byte-identical to their Arland counterparts:

| Anchor | Rorona EN → | Totori EN → | Meruru EN → | Verdict |
|---|---|---|---|---|
| Queue drain | `0x78320` | `0x78320` | `0x78320` | MATCH (12/10/7 votes, reverse CONFIRMS) |
| Text renderer | `0x74bd90` | `0x74bd90` | `0x74bd90` | MATCH (28/28/37 votes, runner-up 0) |
| Atlas lock | `0x581420` | `0x581420` | `0x581420` | WEAK vote, corroborated — see 1.5 |
| Atlas unlock (impl) | `0x581470` | — | — | MATCH (27 votes) |

**Escha & Logy DX and Shallie DX do not.** The text renderer produces *no
n-gram matches at all* in either binary, and the queue drain mismatches on
both vote and prologue shape:

| Anchor | Escha EN | Shallie EN |
|---|---|---|
| Text renderer | no n-gram matches | no n-gram matches |
| Queue drain | WEAK, prologue shape DIFFERS (`0x293850`) | MISMATCH — do not trust |
| Atlas lock | `0x3f1dd0`, byte-identical stub, 6 direct callers | `0x3c8ef0`, byte-identical stub |

The shared *middleware* lock stub survives in all three games — it is
byte-for-byte the same 0x19-byte function everywhere — but the text-rendering
layer above it diverged. Escha's stub has 6 direct call sites against
Ayesha's 11, and none of Escha's callers is a text-renderer homolog.

This is consistent with the previously recorded fact that Escha & Logy and
Shallie are UCRT builds with fast menus, and it confirms the earlier decision
to scope the menu work to Ayesha only. **Escha/Shallie carry no known menu
hitch and are not targets for this fix.** Their open items (Escha's shadow
SRV, Shallie's `CreateSamplerState`) are unrelated; see `TODO.md`.

### 1.4 Ayesha is an incremental-link build — this changes how callers are found

Ayesha's `.text` opens with an incremental-link jump-thunk table (`e9 <rel32>`
entries at `0x1000`–`0x18000`, outside `.pdata`). Calls do not reach their
target directly; they call a thunk that jumps to the real function. Neither
Arland build does this.

The practical consequence is a trap for any call-site search: querying a real
function finds only its thunk, and the function looks nearly unused. The atlas
lock resolves this way — a direct search reports one unverified `jmp` from
`0x3abc`, while searching `0x3abc` itself reports 11 genuine call sites.

**Rule for Ayesha: when a call-site search on a `.pdata` function returns a
single unverified `jmp` from a low RVA, search that thunk address instead.**
The thunk table sits at identical RVAs in the English and multilingual builds
(`0x3abc` reaches the atlas lock in both), which is itself a useful anchor.

### 1.5 Corroborating the atlas lock past its WEAK verdict

The lock is a 0x19-byte leaf; only one n-gram is usable, so `homolog` cannot
do better than WEAK regardless of correctness. Four independent checks agree:

1. **Unanimity** — Rorona, Totori and Meruru all vote to `0x581420`, and every
   reverse vote CONFIRMS.
2. **Prologue and size** — byte-identical to all three Arland locks, same
   `0x19` extent, same `.pdata` bounds, verified function start.
3. **Body shape** — identical instruction-for-instruction to Meruru's, modulo
   the call displacement, which in Ayesha targets a thunk (`0x17b7f` →
   `0x580f90`) where Meruru calls the implementation directly (`0x3ea4a0`).
4. **Caller shape** — resolved through the thunk, Ayesha has **11 call sites,
   exactly matching Meruru's 11**, and one of them (`0x74c020`) lies inside
   `0x74bd90`, the independently MATCH-verified text renderer.

Point 4 is the load-bearing one: the lock is called from the text renderer, in
the same relationship the Arland fix depends on.

### 1.6 The unlock has two levels in Ayesha; hook the stub

Ayesha exposes both shapes the Arland project encountered:

```
0x581460   mov r8d, edx ; xor edx, edx ; jmp 0x136b   -> thunk -> 0x581470
0x581470   the 0x159-byte implementation
```

`0x581460` is the exact homolog of Meruru's hooked unlock (`0x3ea7f0`,
byte-identical shape); `0x581470` is the homolog of Rorona's (`0x3eea60`,
matching `0x159` size). The Arland project hooks the stub on Totori/Meruru and
the implementation on Rorona.

Coverage is equivalent — the implementation is reachable only through the stub
(`0x136b`'s sole user is `0x581465`) — so the choice is about argument
convention. **This repository hooks the stub (`0x581460`)**, so the hook sees
the same `(rcx, rdx=0, r8d=mode)` convention as the existing Meruru/Totori
path rather than Rorona's. Its callers resolve through thunk `0x1163`: 14
sites, including `0x74c09c` inside the text renderer — pairing with the lock's
`0x74c020` as the rasterize-then-read-back round trip the cache exploits.

The stub's 16-byte window contains the `jmp rel32` displacement, so it needs a
**per-build** expected array (Arland precedent: `dtorExpectedEn/Multi`).

### 1.7 Verified Ayesha address pack

Both builds, every prologue byte-verified in the target binary.

| Executable | SHA-256 | `.text` |
|---|---|---:|
| `Atelier_Ayesha_EN.exe` | `b10a07e494abfb5f5711aeb9151b894662b105a3478ac245bf38604d5a6e360d` | `0x984df4` |
| `Atelier_Ayesha.exe` (multilingual) | `95437b9fa746af9c0947a81afd6a671877dc30d22742dd2de197e054202cef22` | `0x9a9604` |

| Anchor | EN | Multilingual | EN→ML verdict |
|---|---:|---:|---|
| Queue drain | `0x078320` | `0x07a8d0` | MATCH, 29 votes, prologue identical |
| Text renderer | `0x74bd90` | `0x76e290` | MATCH, 37 votes, runner-up 0 |
| Atlas lock | `0x581420` | `0x5a3920` | WEAK vote; corroborated, 11 callers via thunk `0x3abc` in both |
| Atlas unlock (stub, hooked) | `0x581460` | `0x5a3960` | stub at lock+`0x40` in both; per-build array |
| Atlas unlock (impl) | `0x581470` | `0x5a3970` | MATCH, 31 votes |

Prologues (16 bytes). Queue drain, text renderer, lock and unlock-impl are
build-independent; the unlock stub is not.

```
queueDrain      48 8b c4 55 41 54 41 55 41 56 41 57 48 8d 68 98
renderText      48 8b c4 48 89 50 10 53 48 81 ec 90 00 00 00 48
atlasLock       48 83 ec 38 44 89 4c 24 20 45 8b c8 45 33 c0 e8
atlasUnlockImpl 48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57
atlasUnlockStub 44 8b c2 33 d2 e9 01 ff a7 ff cc cc cc cc cc cc   (EN)
atlasUnlockStub 44 8b c2 33 d2 e9 01 da a5 ff cc cc cc cc cc cc   (multilingual)
```

The lock's window ends on the `e8` call opcode and deliberately excludes the
displacement, which is why it is portable across builds.

Each row was derived by matching the corresponding Arland entry point into the
Ayesha English build, then matching English to multilingual; the lock's and
unlock's callers were enumerated through their jump thunks (§1.4). The Arland
source addresses used were Meruru's queue drain, text renderer and atlas lock,
and Rorona's atlas unlock implementation.

### 1.8 What the static mapping did and did not settle

The mapping proves the *code path* exists in the same shape. On its own it did
**not** prove the hitch has the same *cause* in Ayesha: how many candidate reads
occur, whether they are redundant, what they cost, and whether Ayesha's drain
brackets menu construction the way Totori's and Meruru's do were all open.

That mattered, because the Arland investigation repeatedly disproved plausible
static hypotheses with a single instrumented run — Rorona's out-of-drain batch,
which forced the frame-scoped lifetime, was found that way and could not have
been predicted from addresses.

§2 answered these by measurement, and the answer was not the one the static
picture suggested: Ayesha turned out to be the frame-scoped case, and to have a
second problem the Arland menu fix does not address at all.

---

## 2. Diagnostic pass

`DUSK_ATLAS_STATS=1` installs the four verified hooks on a recognized Ayesha
build in **counting mode only**: no snapshot is taken, no read is served from
cache, no write is suppressed. Each hook forwards immediately to the original.

Per queue drain it logs to `dusk-fix.log`:

- drain wall time;
- candidate atlas locks (512×512 while the text renderer is active), split by
  access mode (write-map to rasterize vs read-map to blit back);
- distinct middleware texture objects touched, and repeat count per object;
- locks that arrived *outside* any drain (the Rorona-class case that decided
  Arland's frame-scoped lifetime);
- text-renderer invocations and distinct strings rendered.

That is the evidence needed to choose between "queue-scoped cache, as
Totori/Meruru" and "frame-scoped, as Rorona", or to conclude the hitch is
elsewhere and the port is not worth doing.

Reproduction: open Status and Synthesis on the English build, which are the
operations the Arland numbers were measured on, so the two are comparable.

### 2.1 First measured run (English build)

The pattern reproduces, and Ayesha is worse than any Arland game.

| Drain | Wall time | renderText | Candidate locks | read / write | Distinct textures |
|---|---:|---:|---:|---:|---:|
| #658 | 39.2 ms | 18 | 342 | 114 / 228 | 3 |
| #762 | 266.7 ms | 262 | 2385 | 795 / 1590 | 3 |
| #943 | 248.4 ms | 262 | 2385 | 795 / 1590 | 3 |

Findings:

- **The dimension offsets hold.** 100% of candidate locks report 512×512, so the
  `+0x40/+0x42` reads carried over from the Arland middleware are correct in
  Ayesha and the counts above are trustworthy.
- **Three atlases, thousands of locks.** 2385 locks land on 3 distinct
  middleware textures, split almost evenly (worst repeat 798). This is the
  Arland shape exactly — Totori made 2,331 candidate reads against 3 atlases,
  Meruru 3,030 — so the same collapse to three real reads is available.
- **The cost is larger here.** Arland's worst comparable drains were 82–117 ms;
  Ayesha's repeated drain is 248–267 ms. #762 and #943 are the same operation
  repeated with byte-identical counts, which is the signature of work that does
  not depend on changing state.
- **Locks and unlocks balance exactly** (2385 + 262 = 2647), confirming the
  unlock stub hook observes the same population as the lock hook.
- **The read/write ratio differs from Arland.** Ayesha issues two write-mode
  locks per read-mode lock, where the Arland text renderer was documented as one
  write (rasterize) plus one read (blit back) per glyph. Both modes are served
  from a snapshot in the Arland design, so a port would still collapse them, but
  the extra write mapping is unexplained and should be understood before relying
  on that.
- **Exactly one non-candidate lock per renderText call** (18/18, 262/262),
  most likely a probe call with a null output pointer. Harmless, but the 1:1
  correlation is too clean to be coincidence and may identify a size-query
  entry point worth knowing about.

### 2.2 Why the first run could not settle the lifetime

The first run produced **no out-of-drain report at all**, which looks like
evidence for the queue-scoped (Totori/Meruru) lifetime. It is not.

Ayesha reaches its swap chain through `D3D11CreateDevice` followed by
`IDXGIFactory::CreateSwapChain`, not `D3D11CreateDeviceAndSwapChain`. The first
build hooked only the latter, so Present was never hooked, the frame tick never
fired, and the out-of-drain counters accumulated without ever being flushed or
logged. The absence of those lines means "never looked", not "nothing there" —
and out-of-drain locks are precisely what decides between the queue-scoped
lifetime and Rorona's frame-scoped one.

Both routes are hooked as of the follow-up build (the log now reports
`CreateSwapChain hook installed`). This question stays open until a run with that
build reports the out-of-drain line.

### 2.3 Second run: the lifetime is settled, and there are two problems

With both swap-chain routes hooked, the frame boundary fires and the picture
changes substantially. Session: main menu only, opened repeatedly.

| Scope | Frames / drains | Candidate locks | Share |
|---|---:|---:|---:|
| Out of drain | 95 frames | 18,876 | 72% |
| In drain | 4 drains | 7,497 | 28% |

**The cache lifetime is decided: Ayesha needs Rorona's frame-scoped lifetime.**
Most font-atlas traffic happens outside the queue drain, so the queue-scoped
Totori/Meruru lifetime would miss the majority of the work. The first run's
apparent absence of out-of-drain activity was the unhooked-Present artifact
described in §2.2, not a measurement.

The in-drain figures reproduced exactly across both runs — three drains at 2385
candidate locks and 248 ms — so that operation is deterministic and the two
sessions are comparable on it.

**The out-of-drain traffic is not menu construction.** 84 of the 95 frames carry
*identical* counts: 132 candidate locks, `renderText=2`, at ~12.3 ms intervals
(~81 fps). That is two strings re-rendered from scratch every frame, unchanged,
for as long as the menu is on screen — 66 candidate locks per string per frame.
The maintainer confirmed the session was the main menu only, with no field-map
conversation involved.

This is mechanically the same defect the Arland project found in Meruru's
conversation balloon — a per-frame re-entry into the text-render path for text
that has not changed — but the trigger here is the **main menu itself**, which
makes it a far more commonly hit path than Meruru's balloon was.

So Ayesha has two separable problems, needing the two different Arland
mechanisms:

| Problem | Evidence | Arland mechanism that applies |
|---|---|---|
| Menu *construction* cost | 2385 locks / 248 ms per drain, 3 atlases | Atlas read cache, **frame-scoped** |
| Menu *steady-state* re-render | 132 locks/frame, 2 unchanged strings, every frame | Text-bitmap replay cache (`cachedRenderText`), lifetime extended across frames |

The second one needs more than a lift-and-shift: Arland's replay cache was
queue-scoped and had to be widened for Meruru's balloon. Here it must survive
across frames for as long as the menu displays unchanged text, which is a
correctness question about invalidation, not just a lifetime constant.

The largest single event in the log is a frame boundary with `renderText=115`
and 5151 candidate locks, 1,146 ms after the preceding Present.

### 2.4 What this run still does not measure

Every out-of-drain line reports `micros=0`: only the queue drain is timed. So
there are lock *counts* outside the drain but no *cost*, and it remains unproven
that the 132-locks-per-frame drip consumes meaningful frame time. Scaling the
in-drain per-lock rate would imply ~14 ms per frame, which exceeds the observed
12.3 ms frame interval and is therefore unsound — the out-of-drain per-lock cost
must differ. Direct timing of the out-of-drain path, plus the Present-to-Present
interval, is needed before sizing either fix.

---

## 3. Is the Meruru dialog fix transferable?

Partly — the mechanism yes, the trigger no.

The Arland project fixed Meruru's field-map conversation slowdown by widening its
text-bitmap replay cache (`cachedRenderText`, keyed on renderer, font, atlas,
style and the exact string) from queue-scoped to cross-frame, scoped by hooking
the `BalloonBucMode` constructor and destructor so the wider lifetime applies
only while a balloon is live.

**Ayesha has the same class, and both entry points are now resolved.** RTTI
confirms a full balloon family, including `BalloonBucMode` (primary vtable RVA
`0x00bdf0d0` EN, `0x00c1ba08` ML):

| Entry point | Meruru EN | Ayesha EN | Ayesha ML |
|---|---:|---:|---:|
| `BalloonBucMode` ctor | `0x1e8c40` | `0x20cc60` | `0x211a30` |
| `BalloonBucMode` dtor | `0x1e8d30` | `0x20cdc0` | `0x211b90` |

The ctor came from homologue matching (MATCH, prologue byte-identical). The dtor
did **not**: its homologue vote was a reverse MISMATCH, because Ayesha's codegen
pushes `rdi` where Meruru pushes `rbx` (`40 57` vs `40 53`), which is enough to
break the body comparison.

It was resolved instead from **RIP-relative references to the class vtable**,
which is the right discriminator for a constructor/destructor pair: both load
their class's vtable, and nothing else does. Every build has exactly **two** such
references. The method was validated before being trusted — on Meruru it returns
`0x1e8c40` and `0x1e8d30`, precisely the two addresses the Arland project hooks.

Note that a vtable *slot* read would have been the wrong recipe: the hooked
destructor is not virtual, and it is not in the vtable in Meruru either. Ayesha's
vtable slots additionally point at incremental-link thunks (§1.4) rather than
real functions.

Prologues, 24 bytes, **shared across both builds** — the window ends exactly at
the vtable `lea` and so excludes its displacement. (Arland needs per-build arrays
here; Ayesha does not.)

```
ctor  48 89 4c 24 08 57 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 48
dtor  40 57 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 40 48 8b d9 48
```

Both are verified `.pdata` function starts: ctor `0x117` bytes, dtor `0x91`, and
identical sizes in both builds.

Two separate pieces of work, then:

1. **A replay cache for the main menu.** The mechanism transfers directly, but it
   needs a scope signal, and `BalloonBucMode` is the wrong one. Simply making the
   replay cache permanent is not acceptable: the cache key includes the string, so
   stale entries survive only as long as invalidation is provably complete.
2. **The Meruru conversation fix proper.** Both addresses are now resolved and
   verified, so this is implementable as a direct port. Independent of the above,
   and probably a real win on Ayesha's conversations, but still unmeasured — no
   run in this investigation had a field conversation on screen, so the symptom
   is assumed rather than observed.

Note that the atlas cache in §4 already reduces the steady-state cost without
either of these: it collapses the per-frame 132 candidate locks to at most three
real reads per frame. What it does not remove is the CPU-side glyph work above
the atlas, which is what a replay cache would eliminate.

---

## 4. The atlas read cache (ships on by default)

`DUSK_ATLAS_CACHE=1` on Ayesha. Opt-in, off by default, capability-gated to
Ayesha; Escha and Shallie are hard-off.

Structure follows the Arland implementation, with the **frame-scoped** lifetime
(Rorona's), chosen because 72% of Ayesha's candidate locks fall outside the queue
drain (§2.3). The queue drain deliberately neither arms nor clears the cache;
Present owns both, so snapshots survive across drains within a frame and never
across a frame boundary.

Rules carried over from Arland, each for a reason that was paid for there:

- A lock is eligible only while the verified text renderer is on the stack, with
  a real output pointer, on a 512×512 middleware texture.
- **Snapshots are created only from read-mode locks.** A write mapping is
  discard-mapped and its contents undefined on entry; snapshotting one captures
  uninitialized memory and poisons the read that follows. The first candidate lock
  of each atlas is a write, so this is the difference between working and a
  corrupted glyph.
- **Both modes are served** from an existing snapshot. Writer and reader name the
  same middleware object, so one snapshot stands in for the whole
  rasterize-then-read-back round trip; serving reads only would forfeit most of
  the saving.
- Synthetic locks are tracked per thread and each holds its snapshot alive by
  `shared_ptr`, so a clear or invalidation on another thread cannot free a buffer
  a caller is still reading. Only a matching synthetic unlock is suppressed.
- **Any unmatched real unlock invalidates that texture's snapshot.** The glyph
  atlas is a single mutable, demand-paged surface; without this, a glyph paged in
  after the snapshot is served stale and blits blank. This is the Arland
  missing-kanji bug.
- Hooks arm behaviour only after all four install, so a partial install is
  pass-through rather than half-caching.

### 4.1 Measured effect

English build, same operations with the cache off and on:

| Menu build | Cache off | Cache on | Reduction |
|---|---:|---:|---:|
| 2385 candidate locks | 248.4 ms | 38.2 ms | 85% |
| 1527 candidate locks | — | 24.1 ms | — |
| 342 candidate locks | 39.0 ms | 8.8 ms | 78% |

Session totals: 63,517 cache hits against 3,029 real reads, a 95.5% hit rate. For
comparison the Arland cache reduced its equivalent drains by 34–48%; the larger
gain here reflects Ayesha's greater redundancy, not a better cache.

The residual ~38 ms is CPU-side glyph and layout construction above the atlas,
which this cache cannot reach. Arland reached the same conclusion by measurement
there: its remaining drain time was construction, not GPU transfer.

### 4.2 Risk accepted in shipping it on by default

The failure mode is wrong or missing glyphs, and it has **not** been validated by
a playthrough. The decision to ship it on is deliberate and the maintainer's; what
follows is the residual risk it accepts.

The specific untested case is a glyph the atlas must page in mid-frame. The
invalidation rule in §4 exists for it — any unmatched real unlock drops that
texture's snapshot — and it is the rule that fixed the equivalent Arland
missing-kanji bug, so the design anticipates the case. What is missing is
confirmation that Ayesha's write path routes through the same unlock, which is a
runtime question. The multilingual build is the higher-risk one, since uncommon
kanji page in far more often than Latin.

Mitigation if a report arrives: `DUSK_ATLAS_CACHE=0` restores the game's own
behaviour without a rebuild, and the log records the cache's hit/real-read split
for any run.

---

## 5. High-refresh field jitter (ported, unvalidated)

Arland's field-map jitter fix — above roughly 115 fps the character buzzes
vertically while standing on a step or ledge, because a collision-resolver
constant means a per-frame *distance* that was only ever right at 60 fps — ports
cleanly on addresses. Whether the symptom exists in Ayesha has **not** been
measured.

All three anchors resolved, and the threshold check is the strong one:

| Anchor | Ayesha EN | Ayesha ML | How |
|---|---:|---:|---|
| Controller `Update` | `0x739fa0` | `0x75c4a0` | MATCH from Meruru, 33 votes, prologue byte-identical |
| Collision resolver | `0x738670` | `0x75ab70` | MATCH from Meruru, 13/19 votes |
| Move threshold (data) | `0x1627f20` | `0x17bf8e0` | data-section scan for the shipped bit pattern |

The threshold was found by scanning the writable data section for `0x3c0b4396`
(0.0085f). Exactly one occurrence per build, and the method was validated by
reproducing Meruru's known address first. Crucially it has **exactly one
reference in the image**, from inside the prologue-verified resolver
(resolver+`0x5fb`; Meruru's sits at +`0x5d3`), and no writer — which is what makes
rescaling it safe.

Ayesha's resolver prologue differs from the Arland one in its final byte
(`8d a8 d8` vs `8d a8 e8`, a larger frame), so it carries its own expected array.

Both halves ship **OptIn**, unlike Arland where both are on by default, and the
asymmetry between them matters:

- **The rescale** writes one constant whose single reader is verified. Low risk.
- **The stabilizer** writes into the live controller object at offsets
  (`kVelYOffset`, `kAirTimerOffset`) carried over from the Arland builds. Those
  are justified there by the six Update bodies being instruction-for-instruction
  identical. Ayesha's Update matches on prologue and homologue vote, but its
  object layout is **not confirmed**, and a wrong offset corrupts live game state
  rather than failing cleanly.

`DUSK_FIELD_TRACE=1` validates the layout cheaply before anything writes: it
reads the same offsets, so plausible `posY`/`velY`/`grounded` values mean the
layout holds and garbage means it does not. Run that before
`DUSK_FIELD_STABILIZER=1`.

---

## 6. Hook boundaries

Same rules as the Arland project. Hooks install only after the process is
recognized as one of the two fingerprinted Ayesha executables by name and
`.text` size, and every target is byte-checked against its complete expected
prologue before MinHook is invoked. MinHook is used rather than the byte-patch
installer because the hooked unlock is a 14-byte stub.

Escha & Logy and Shallie are recognized for identification and logging, but
have no menu hooks and no mapped addresses; the capability matrix hard-offs
the feature for them so no configuration can turn it on.

Nothing is written to any Koei Tecmo executable; all patching is in-memory and
signature-gated.
