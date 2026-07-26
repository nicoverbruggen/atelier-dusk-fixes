# TECHNICAL

Authoritative record of what has been established about the Dusk DX ports and
what this repository does about it. Same policy as the Arland project: this file
stays in sync with the code, and every address pack records how it was derived.

Status: no runtime-validated fix ships yet. The menu-hitch investigation below
has a complete, corroborated **static** mapping for Ayesha; the diagnostic pass
exists to decide whether the Arland fix applies before any cache is ported.

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

### 1.8 What is NOT established

The static mapping proves the *code path* exists in the same shape. It does
**not** prove the hitch has the same *cause* in Ayesha, and nothing here has
been observed at runtime.

Specifically unmeasured: how many candidate atlas reads Ayesha issues per
queue drain, whether they are redundant, how much wall time they cost, and
whether Ayesha's drain brackets menu construction the way Totori's and
Meruru's do. The Arland investigation repeatedly disproved plausible static
hypotheses with a single instrumented run — Rorona's out-of-drain batch, which
forced the frame-scoped lifetime, was found that way and would not have been
predicted from addresses.

The atlas cache is therefore **not** ported yet. §2 ships the measurement
instead.

---

## 2. Diagnostic pass (what ships now)

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

---

## 3. Hook boundaries

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
