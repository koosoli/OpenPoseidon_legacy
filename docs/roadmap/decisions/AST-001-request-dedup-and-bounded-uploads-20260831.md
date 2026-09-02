# AST-001 — Request deduplication and bounded uploads: an audit, and the four counters it produced

Date: 2026-08-31
Branch: `agent-request-dedup` (from `safety/asset-obs-001`)
Roadmap: critical-path step 6 — *"Implement the cheapest request-deduplication and bounded-upload wins"*;
section 1.3 "Small asset-loading wins", bullets 1 (deduplicate simultaneous requests) and 4 (cap GPU
uploads by bytes and/or milliseconds per frame).

## Verdict first

**Request deduplication was already complete on every asset path.** Nothing was added to it, because
there is no duplicate work left to remove. What was missing was the ability to *see* it, and — more
seriously — the ability to see whether any of the loop bounds that govern streaming ever fire. Five
separate bounds governed the object-admit loop and not one of them reported a single event.

This is the sixth roadmap item this week whose implementation already existed. The audit is the
deliverable; the code is four counters.

---

## Part 1 — The dedup inventory

For each asset request path: what happens when the same asset is asked for twice in quick succession.

| Path | Second request | Where | Counted? |
|---|---|---|---|
| `ObjectStreamPreparer::Request` | Refused; rides on the first parse. Slot state is anything but `Unknown` → return false, caller skips this frame and retries. | `ObjectStreamPrepare.cpp:556` | **Was not.** Now `coalesced` / `coalescedStuck`. |
| `PreparedTextureStore::Put` / `ShouldPrepare` | Refused as a duplicate; the loser's worker read is discarded. This is the one path with genuine producer-producer racing. | `PreparedTextures.cpp:185`, `:225`; `PreparedTextures.hpp:57` | Yes — `rejectedDup`, in panel and JSON. |
| `TextureBankWgpu::Load` | Returns the same `Texture` from the name→index map. A texture is created once per name, ever. | `TextureBankWgpu.cpp:207-217` | No counter, and none is wanted — see below. |
| `TextureWgpu::EnsureUploaded` | Idempotent: `if (_gpuHandle \|\| _uploadTried) return _gpuHandle;`. One upload attempt per texture per session. | `TextureWgpu.cpp:1292` | No counter, and none is wanted. |
| `ShapeBank::NewFromModel` | Returns the cached shape (`_cache.Lookup(cacheKey)`). Also carries a re-entrancy guard for proxy cycles, which is dedup against *itself* further up the stack. | `ShapeDraw.cpp:631`, `:650` | `LoadTiming::cacheHit` per call, not aggregated. |
| `FileCache::Load` (the file server) | MRU cache hit; shares the decompressed bytes. | `FileServer.cpp:103-113` | Yes — `hits` / `misses` / `missUs`, in panel and JSON. |

### Why three of these need no counter

The last four rows are all **main-thread-only by assertion**, not by convention:
`POSEIDON_MAIN_THREAD_ONLY` guards `FileCache::Find` (`FileServer.cpp:46`) and
`ShapeBank::NewFromModel` (`ShapeDraw.cpp:614`), and the texture bank is reached only from the same
thread. On a single thread there is no such thing as a *simultaneous* request: a second ask is
strictly later than the first, the first has already published its result into a name-keyed map, and
the second is a hash lookup. Dedup there is not a mechanism that could fail silently — it is the data
structure. Counting it would produce a number equal to "cache hits", which is already reported for
the one of them where it costs anything (the file cache).

Genuine concurrency exists in exactly two places, and both were already instrumented or are now:
worker-vs-worker on `PreparedTextureStore` (`rejectedDup`, pre-existing) and main-thread-vs-worker on
`ObjectStreamPreparer` (added here).

**Conclusion: roadmap 1.3 bullet 1 is done, and was done before this session started.** No code was
written for it.

---

## Part 2 — The bounds inventory

What is bounded, where the bound is enforced, and — the useful column — whether anything reports the
bound biting.

| Bound | Value | Enforced at | Bite reported? |
|---|---|---|---|
| Preparer queue depth | 512 (`WGR_OBJECT_STREAM_ASYNC_QUEUE`) | `ObjectStreamPrepare.cpp:558` | Yes — `rejectedFull`, panel + JSON. |
| Preparer ready set | 128 (`WGR_OBJECT_STREAM_ASYNC_READY`) | `ObjectStreamPrepare.cpp:179` — workers *park*, they do not drop | **No.** Added: `readyFullParks`. |
| Prepared-texture store bytes | 512 MB (`..._TEXTURE_PREPARE_MB`) | `PreparedTextures.cpp:185` | Yes — `rejectedFull`. |
| Prepared-texture store TTL | 30 s | `PreparedTextures.cpp` sweep | Yes — `expired`. |
| Admit hard cap | `creates * scale` | `LandSave.cpp:3487` | **No.** Added: `admitStopCap`. |
| Admit upload byte ceiling | `WGR_OBJECT_STREAM_UPLOAD_MB`, **default off** | `LandSave.cpp:3479-3490` | **No.** Added: `admitStopUpload` (armed-gated). |
| Admit wall-clock ceiling | 16 ms (`WGR_OBJECT_STREAM_MAX_MS`) | `LandSave.cpp:3507`, `:3509` | **No.** Added: `admitStopCeiling` (armed-gated). |
| Admit ordinary time budget | `WGR_OBJECT_STREAM_BUDGET_MS` | `LandSave.cpp:3511` | **No.** Added: `admitStopBudget`. |
| Resident object count | 20,000 | residency update | Gauge only (`resident / budget`); no refusal counter. Left alone — see "not done". |
| Per-frame texture upload | measured, **not capped** | `TextureWgpu.cpp:1444` accumulates; `StreamingFrameRoll` rolls | Measured (`peakFrame*`), and the only enforcement of it is the admit ceiling above. |

The brief supposed "the texture upload path has a per-frame budget somewhere". It does not, quite:
`TextureWgpu` only *accumulates* frame bytes. The single consumer of that accumulator is the admit
loop's upload ceiling (`LandSave.cpp:3482`), which stops admitting objects — it never refuses an
upload. So a frame that uploads 36 MB outside admission is unbounded and always has been. That is
correct as designed (a UI first-bind must not be deferred), but it is worth stating plainly rather
than leaving "there is a budget" on the record.

### The finding that mattered

`admitBudgetSpent()` (`LandSave.cpp:3485`) folds four bounds and a floor into one `bool`. The loop
stops; nobody records why. The consequence is on the record already: the upload ceiling ships
**default-off** because an A/B on the Everon traverse "changed NOTHING" (the long comment at
`LandSave.cpp:2670`). That A/B cost two whole captures to answer a question a stop-reason tally
answers directly and for free — *did that ceiling stop a single update?* Every future tuning of the
floor, the cap or the ceilings faced the same cost.

---

## Part 3 — What was built

Four counters, in the repo's existing plain-struct/`StreamingDiag` pattern, surfaced in both the dev
panel's Streaming tab and the `--capture-metrics` JSON sidecar so the two cannot disagree.

**`ObjectStreamPreparer::Stats`** (`ObjectStreamPrepare.hpp`, incremented `ObjectStreamPrepare.cpp:556`, `:179`):

- `coalesced` — asks refused because the model was already Queued/Parsing/Ready/Converting/Converted.
  This is the dedup win: `coalesced / (coalesced + requested)` is the share of asks that cost nothing.
- `coalescedStuck` — asks refused because the model is sticky `NotLoose` or `Failed`. **Kept separate
  on purpose.** These callers fall through to the synchronous main-thread path, so folding them in
  would report a packed world — where every model is `NotLoose` — as a ~99% dedup rate while nothing
  was being deduplicated at all.
- `readyFullParks` — a worker parked with queue work in hand because the ready set was at its limit.
  Counted under the mutex immediately before the wait, and **only** when the queue is non-empty: an
  idle worker waiting on an empty queue is "nothing to do", not back-pressure, and counting it would
  make every quiet frame look like a bound biting.

**Admit-loop stop reasons** (`Landscape.hpp` members, `LandSave.cpp` `admitBudgetSpent`): `admitUpdates`
as the denominator plus four disjoint counters, recorded on the *first* bound to fire in an update.
First-only matters: the outer cell loop keeps re-asking after a bound is spent, so counting every
`true` would measure how many cells the world has. Updates that drained every candidate are the
remainder, `admitUpdates - sum`.

### The n/a discipline

Both ceilings can be switched off, and a disabled bound reports the same `0` as an armed bound that
was never reached. Those are opposite conclusions — *this lever is not needed* versus *this lever is
not on* — and the upload ceiling is default-off, so the confusing case is the default case.

- JSON: `admit_stop_ceiling` and `admit_stop_upload` **keys are absent** unless armed.
- Panel: the rows read `n/a (WGR_OBJECT_STREAM_UPLOAD_MB unset)`.
- The `*Armed` flags are read from the same accessors the admit loop itself calls
  (`ObjectStreamUploadBudgetBytes()`, `ObjectStreamCeilingMs()`), so a consumer's n/a cannot drift
  from what the loop did.

This follows the two defects fixed the same way earlier on this branch (the dead `cpu_milliseconds`
column, the snapshot drop counter's assumed value) and the existing `per_object_kb` precedent.

### Tests

Two cases, +25 assertions, in `tests/unit/engine/Poseidon/Dev/Diag/test_streaming_diag.cpp`. Both
drive a real `ObjectStreamPreparer` over the `complex_vehicle.p3d` fixture rather than poking
counters, because the increments live inside the lock that reads the state — a test that set the
field directly would pass with the counter never wired.

- Six asks, one enqueue: `requested == 1`, `coalesced + coalescedStuck == 5`, `coalescedStuck == 0`
  (the fixture is loose and parseable, so no refusal may be the sticky kind). Out-of-range asks move
  neither counter. After `Take()` returns the slot to `Unknown` the next ask is a real request again
  — dedup that never expired would be a leak, not a saving.
- A run that never approaches the ready limit leaves `readyFullParks == 0`. This is the negative test
  the park counter needs: a counter that ticked on ordinary empty-queue waits would be worse than no
  counter, because every quiet frame would read as back-pressure.

Before: 3033 cases / 241,798 assertions / 0 failures.
After: 3035 cases / 241,823 assertions / 0 failures.

---

## Part 4 — What was deliberately not done

- **No new dedup.** Part 1 found none missing. Adding a coalescing layer over a name-keyed map on a
  single thread would be pure cost.
- **No per-frame texture upload cap.** The evidence to justify one does not exist and the evidence
  that exists points away: the worst measured frame was *one object* (church_01.p3d, 43 textures),
  and the residual tail of any admission ceiling is `budget + one object` by construction. The
  counters added here are exactly what would tell us if that changes on another corpus. Building the
  cap now would be a second lever without a measured problem, next to the first one.
- **No counter on the 20,000-object residency budget.** It is a gauge already (`resident / budget`)
  and the interesting question there is bytes, not items — which `GpuResidencySnapshot` /
  `bytesPerResidentObject` already answers.
- **No counters on the main-thread-only banks.** Argued in Part 1; they would duplicate cache-hit
  numbers that already exist where they cost something.
- **Nothing was measured in-game.** Another session owns the game folder. Every claim here is from
  code or from unit tests; the numbers these counters will produce are not in this document, and the
  first capture that reads them should be treated as the first data, not as confirmation.

## Is critical-path step 6 done?

**Bullet 1 (deduplicate simultaneous requests): yes, and it was done before this session.**
**Bullet 4 (cap GPU uploads by bytes/ms per frame): the cap exists, ships off, and now reports
whether it fires — which is the gate on deciding its default.** Turning it on is not a code change;
it is a capture, and it needs somebody who owns the game folder.

Step 6 should be marked done *with the audit attached*, not re-attempted. The one open action is a
traverse capture on Everon reading `admit_stop_*` with `WGR_OBJECT_STREAM_UPLOAD_MB=16`, which would
settle the default-off decision at the cost of one run instead of two.


---

## Addendum, same day: the traverse this record asked for

AST-001 left one open action for whoever owns the game folder — an Everon traverse with
`WGR_OBJECT_STREAM_UPLOAD_MB=16` reading `admit_stop_*`, on the claim that it *"settles the
default in one run instead of two"*. Run: `ev_DALL.wrp`, 200,000-object budget, freefly,
screenshot at 35 s, **three runs per arm**.

| | resident objects | `admit_stop_upload` |
|---|---|---|
| cap off (default) | 76,719 / 68,540 / 101,323 — mean **82,194** | key absent, all three |
| cap 16 MB | 119,312 / 75,776 / 87,114 — mean **94,067** | **19 / 18 / 17** |

**Half the claim holds, and it is the important half.** The stop-reason tally is stable to
±1 across runs and settles *"does the cap ever fire"* — 17 to 19 times a traverse —
definitively, in one run, where before it took two full captures to conclude "changed
NOTHING". The counter did exactly what it was built for.

**The other half does not.** *"Should the default change"* is not settled and three runs per
arm cannot settle it: the spreads (68k–101k against 76k–119k) overlap almost completely, so
the 12k gap between the means is not distinguishable from noise on this scene. That is not a
failure of the counters — it is the difference between a question about the code, which a
counter answers, and a question about the world, which needs a quieter measurement than this
scene offers. **The default stays off** until somebody makes that measurement; the honest
statement today is "the cap works and we do not know whether it helps".

The n/a discipline also held under test: with the cap off the JSON key is **absent**, not
zero, in all three runs.

> **Stale lever, verified 2026-09-02 (AUD-001):** `WGR_OBJECT_STREAM_BUDGET_MS` has NO READER IN THE TREE — `rg -w` over engine/, apps/, tools/, tests/, scripts/ returns nothing. Setting it does nothing. The live name is `WGR_OBJECT_STREAM_MS_PER_UPDATE`. Do not run a confirming experiment with it.
