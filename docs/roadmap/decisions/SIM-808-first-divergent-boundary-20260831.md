# SIM-808 — box3d's replay answers a different question, so the first divergent boundary was built; box3d's is wired beside it (2026-08-31)

Roadmap **Phase 8.6** and **Phase 13** — *"record/replay should report the first divergent
authoritative state boundary"*, strict comparison for the same build and platform, tolerance
confined to documented cross-platform diagnostics that can never hide a divergent discrete
outcome. Branch `agent-record-replay`, based on `safety/asset-obs-001`; baseline measured at
`48a26cff`, rebased onto `5ec56657` before the first commit landed.

**Related:** [SIM-804](SIM-804-physics-integration-boundary-20260831.md) (the physics boundary
audit, which found box3d's replay facility and named it as the roadmap item),
[SIM-806](SIM-806-worker-count-determinism-harness-20260831.md) (whose method this reuses),
[SIM-803](SIM-803-stable-iteration-and-tie-breaking-20260831.md),
[SIM-801](SIM-801-worker-fp-environment-20260831.md).

## Decision

**Build ours, and wire box3d's beside it as a scoped second witness.** Not either/or: they
answer different questions, and the failure mode this record exists to prevent is somebody
reading box3d's green tick as the roadmap item being closed.

## First: what box3d's facility actually does

Read from source at `C:\vcpkg\buildtrees\box3d\src\v0.1.0-4f43a52fd7.clean` —
`src/recording.c`, `src/recording_replay.c`, `src/recording_ops.inl`, `src/world_snapshot.c`,
about 7,000 lines. It is always compiled in; there is no `B3_ENABLE_RECORDING` gate.

**What it records.** `b3World_StartRecording` writes a header, then a **complete world
snapshot** (`b3SerializeWorld`), then every subsequent box3d API call against that world as a
typed op. The op table is `recording_ops.inl`; `Step` is op `0x37`-ish among ~230 others, and
`StateHash` is op `0xF1`. The buffer grows on demand (`b3RecBufAppend` doubles), so the
`byteCapacity` argument to `b3CreateRecording` is a hint and not a cap.

**What it hashes.** `b3HashWorldState` (`recording.c:1232`) walks the body table, skips free
slots, and mixes — as raw IEEE-754 **bits**, via `memcpy`, never as values — each live body's
`transform.p`, all four quaternion components, and `linearVelocity` / `angularVelocity` where a
`b3BodyState` exists. FNV-1a, 64-bit. **That is the whole state vector.** No contacts, no
manifolds, no island membership, no sleep flag, no joints, no shape parameters, no user data.

**When.** Once at `StartRecording`, so replay is checked from the first step
(`recording.c:1074`), and once after every `b3World_Step` (`physics_world.c:1170`).

**What it reports on divergence.** `b3RecDispatch_StateHash` (`recording_replay.c:1628`)
recomputes the hash in the replay world and compares it for **exact equality** —

> `if ( computed != a->hash ) { printf( "b3ReplayFile: StateHash mismatch ..." ); rdr->diverged = true; }`

— which is a `printf` to stdout of the two 64-bit hashes, and a latched flag. The player then
exposes `b3RecPlayer_GetDivergeFrame()`, the **frame** of the first mismatch.
`b3ValidateReplay` is a four-line convenience wrapper that throws all of that away and returns
a `bool`.

**So: it names the frame. It names nothing inside the frame.** With one 64-bit hash over every
body's pose and velocity, there is no information in the report that could name a body, let
alone a field. Its bit-exactness and its refusal of tolerance are exactly right; its
granularity is one step coarser than SIM-804's whole-run hash and no finer.

**Cost, measured** (below): 31,489 bytes for 60 steps of a 24-dynamic-body world, ≈525
bytes/frame. Replay is a full re-simulation.

**The one thing it does that nothing else here can.** `b3RecPlayer_Create(data, size,
workerCount)` builds the replay world at a **chosen** worker count, clamped to
`[1, B3_MAX_WORKERS]` (32). box3d's own header says replaying at a different count
*"re-partitions the constraint graph"*. SIM-804 measured our live `workerCount` as **1**, so
box3d's multi-worker solver has no coverage from any test that steps the live world, and no
way to acquire any.

## Second: wire it, or build ours?

**Both, and the reason is that box3d's subject is not the roadmap's subject.**

The roadmap's "authoritative state" is event order, selected targets, hits, deaths, ownership
changes and replicated state. box3d's replay **cannot see any of it, by construction** — the
engine's own calls are *baked into the recording*. Replay re-issues `SetBodyTransform`,
`ApplyImpulse` and `Step` in the order the engine already chose and asks whether box3d, given
those inputs, produces the same bodies. A divergence **upstream** of the call — a different
target selected, a different unit stepped first, a different impulse computed — is not a
divergence box3d's replay can report. It is a divergence box3d's replay *faithfully
reproduces*.

Put plainly: box3d's facility answers **"is box3d deterministic against itself"**. The roadmap
asks **"is our simulation deterministic, and where does it stop being so"**. Wiring the first
and calling the second done would be the most expensive kind of mistake available here — a
green oracle over the wrong subject.

The counter-argument for wiring it is real and is why it was wired: it sees **deeper** than the
engine ever can. The engine's boundary hands out three scoped readbacks (SIM-804's table);
box3d's hash is taken *inside*, over its own body table, and the replay exercises the solver's
internals. And it reaches the worker counts we cannot. So: **narrower in subject, wider in
depth, and cheap** — which is the definition of a second witness rather than an oracle.

## What was built

### The engine side — `engine/Poseidon/Core/StateTimeline.hpp` / `.cpp`

A run is recorded as an **ordered stream of labelled fields, per tick**:

```
BeginTick() ; F32("probe", 7, "pos.y", v) ; Bool("probe", 7, "awake", b) ; ... ; EndTick()
```

`CompareTimelines(a, b)` returns the **first** place they differ, as one of four kinds — they
are different failures and collapsing them would send a reader after the wrong thing:

| Kind | Meaning |
|---|---|
| `FieldValue` | same field, different bits |
| `FieldIdentity` | same slot, different fact — an **ordering** divergence |
| `RecordShape` | a record appeared or vanished |
| `TickCount` | one run ended early |

`FieldIdentity` is how "event order" gets covered rather than asserted. Comparison is
positional, so two runs that emitted the same facts in a different sequence are reported as
having diverged in *order*, naming both sides of the slot.

Three properties the gate turns on:

- **Bits, never values.** SIM-806's rule, restated: `-0.0f == 0.0f` and `NaN != NaN`, so a
  value comparison is simultaneously too loose and too tight. A case pins both halves.
- **No tolerance, and no parameter for one.** `CompareTimelines` takes two timelines and
  nothing else.
- **`FieldKind::Discrete`.** Every field carries Continuous or Discrete. The gate's wording is
  that tolerance is a cross-platform *diagnostic* that can never hide a divergent **discrete**
  outcome, so a discrete field is tagged at the point of recording and the report says so. The
  `ulpDistance` on a result is computed **only** for a continuous 32-bit field, is `-1`
  otherwise, and is read by nothing — the verdict is already decided when it is filled in. A
  discrete divergence prints *"a discrete outcome differs; no tolerance applies"*.

The header is **std-only**. It is not a physics facility; physics is merely the only
authoritative simulation job that exists (SIM-804, SIM-806 both say so). Event order, target
selection and replicated state are the same shape and should record into the same timeline.

Labels are interned; the record key is the fixture index, **never a `BodyId` and never an
address** — SIM-803 is an entire decision record about an ordering derived from allocator state.

### The backend side — `ReplayAudit` on `PhysicsBackend`

Two virtuals with default implementations that report `supported == false`, so a future backend
need not implement them: `BeginReplayRecording()` and
`EndReplayRecordingAndValidate(int replayWorkerCount)`. `Box3DBackend` implements them over
`b3CreateRecording` / `b3World_StartRecording` / `b3RecPlayer_*`.

`b3ValidateReplay` is deliberately **not** used. It returns a `bool` and discards the frame,
which is the one number this whole item is about; the player is driven directly so
`divergentFrame` survives.

`ReplayAudit` carries `ran` because a replay facility that returns a cheerful
`diverged == false` when it validated nothing is worse than no facility. `ran` and `frameCount`
are `REQUIRE`d in the tests, not `CHECK`ed.

`Destroy()` now stops and frees a live recording **before** destroying the world — the
recording holds a pointer into it.

### The tests — `tests/unit/engine/Poseidon/World/test_first_divergent_boundary.cpp`

14 cases, 893 assertions, tags `[determinism][replay]` (physics cases also `[physics]`), in
`PoseidonTests`. **No game data.**

The physics fixture is SIM-804's, parameter for parameter — 16×16 bowl heightfield, one static
convex hull, 24 probes of four shapes in a cluster over it, 240 steps at 1/60 s — deliberately,
so the two harnesses describe the same simulation and the two write-ups can be read side by
side. A cluster and not a scatter: bodies that never touch measure only the integration of
gravity, and the contact graph is what a worker count re-partitions.

## Ablation evidence

**A test that cannot fail is worthless.** The perturbation is applied to the **live world** —
one impulse, one body, one tick — not to a recorded stream. Forcing the tick assertion to fail
prints what the harness reported:

```
ablating one body at one tick names that tick and that body
  CHECK( d.tick == 999999 ) with expansion: 120 == 999999
  with message:
    first divergence at tick 120, entry 38: sample[7].pos.x (continuous)
    0x42955FE0 vs 0x42955FE2 (74.687256 vs 74.687271), 2 ulp

an ablation at the last tick is reported at the last tick
  CHECK( d.tick == 999999 ) with expansion: 239 == 999999
  with message:
    first divergence at tick 239, entry 19: sample[3].pos.y (continuous)
    0x3EBFFA46 vs 0x3ED1F55A (0.374956 vs 0.410075), 1178388 ulp
```

The tick, the record, the field, both bit patterns, both decoded floats, and the ulp distance
as a trailing diagnostic. **Two ulp** at tick 120 is the interesting number: that is the size
of the first observable consequence of a 2 N·s impulse, and it is reported as a **divergence**,
not as agreement — which is what "no tolerance" has to mean in practice.

The second case is the control on the first. A comparison that always reported the tick it was
handed, or always an early one, would pass the first case; moving the perturbation to the last
tick and requiring the answer to move with it is what makes "first" mean first. It also pins
that a divergence in the final tick is not lost because the run "mostly agreed" — and the test
additionally requires every earlier tick hash to be equal, so the shared prefix really was
shared.

Committed, both restored to `CHECK(d.tick == kAblationTick)`, and both pass.

The comparison's other three kinds are ablated on hand-built timelines, where the perturbation
can be exactly one thing: one ulp on one field at one tick (`FieldValue`, and `ulpDistance == 1`
while the verdict stays `Diverged`); a changed `targetId` (`FieldValue` + `Discrete` +
`ulpDistance == -1`); two events swapped from tick 2 on (`FieldIdentity`); an extra record at
tick 4 (`RecordShape`); a run five ticks long against one seven ticks long (`TickCount`).

## The box3d arm, measured

Same fixture, 60 steps, recorded at the live worker count (1) and replayed at three:

| replay workers | frames | recorded bytes | diverged | `divergentFrame` |
|---:|---:|---:|---:|---:|
| 1 | 60 | 31,489 | no | -1 |
| 2 | 60 | 31,489 | no | -1 |
| 4 | 60 | 31,489 | no | -1 |

**box3d 0.1.0 is bit-identical across 1, 2 and 4 workers on this fixture.** Recorded here as a
fact about this fixture and this version, **not** asserted as a guarantee — pinning it would
make the next box3d responsible for keeping an accident true, which is the mistake SIM-804 named
about FTZ/DAZ. It is nonetheless the first evidence in the tree that raising `workerCount` would
not immediately cost determinism, and if it ever changes, `divergentFrame` names the step.

Cost: ≈525 bytes per frame for 24 dynamic bodies. Nothing in a shipping frame touches this.

## What the two facilities can and cannot see

| Authoritative state the gate names | Engine timeline | box3d replay |
|---|---|---|
| Rigid-body pose and velocity | **yes**, per body per tick, named | yes, one hash per frame |
| Sleep / awake transitions | **yes**, as a Discrete field | no |
| Body appearing or vanishing | **yes** (`RecordShape`, and a Discrete count) | partially — the hash walks live slots |
| Contacts, islands, constraint-graph colouring | no | **yes**, indirectly, via the solver's output |
| Behaviour at worker counts > 1 | no | **yes** — the only thing that can |
| Event order | **yes** (`FieldIdentity`), untested on real events | **no, by construction** |
| Selected targets, hits, deaths | **shape supports it**, nothing records it yet | **no, by construction** |
| Ownership changes, replicated state | same | **no, by construction** |
| Terrain generation | SIM-806's harness, separately | no |

## What is NOT covered

- **No AI, scripting, network or gameplay state is recorded.** The timeline's *shape* takes it
  — that is the point of a std-only header keyed by stream/key/field — but **nothing calls it
  from those subsystems**, and until something does, Phase 8.6's and Phase 13's language about
  "event order, selected targets, hits, deaths, ownership changes or replicated state" is
  **not** satisfied for any of those. The ordering divergence is proved on synthetic events,
  not on real ones. This is the largest gap and it is the next slice.
- **There is no record/replay of the ENGINE.** Both runs here are live re-simulations compared
  after the fact. Nothing serializes a run to disk, nothing replays one, and there is no
  input-log format. "Record/replay" in the roadmap's fullest sense — capture a session, replay
  it later on another build — remains unbuilt. What exists is the *comparison* half, which is
  the half that had nothing.
- **box3d's StateHash check is not ablated.** `ran` and `frameCount` rule out "the replay
  validated nothing at all", but no divergence could be constructed inside box3d 0.1.0 on this
  fixture, so that the hash comparison would *catch* one is established by **reading
  `b3RecDispatch_StateHash`**, not by making it fail. Stated as a source-level verification and
  not as a measurement.
- **No cross-platform comparison, and therefore no tolerance policy in force.** Everything is
  same-build, same-platform, strict equality. `FieldKind` and `ulpDistance` are the *scaffolding*
  a cross-platform diagnostic would need — they are not a diagnostic, and no tolerance path
  exists to be misused yet. When one is written, the rule it must obey is already encoded: a
  `Discrete` field carries no distance at all.
- **No TSan run.** Same gap SIM-804 and SIM-806 both record: the preset is Linux-only
  (`linux-x64-clang-tsan`) and this session is on Windows.
- **The fixture is synthetic.** Real Geometry LODs need `ExtractPieces`, a `LODShape` and
  therefore game data. The convex-piece path is the production one; its inputs are generated.
- **The engine's live `workerCount` was not raised.** The box3d arm measures that replaying at
  2 and 4 agrees; it does not measure the engine *running* at 2 or 4, and nothing here
  recommends it. `b3World_SetWorkerCount` is still never called.
- **Memory is unbounded by design.** A timeline keeps every field of every tick — 240 × ~600
  entries ≈ 3 MB for this fixture. That is right for a test and wrong for a long session; a
  ring buffer or a hash-first/expand-on-mismatch mode is what a live capture would need.
- **Nothing was validated in the game, and nothing was deployed.** Two other sessions need the
  single allowed instance, so verification here is build plus unit tests. Everything added is
  inert in a stock frame: the timeline has no caller outside tests, and `BeginReplayRecording`
  is a switch a test throws. The only change on a path the game runs is `Box3DBackend::Destroy`
  stopping a recording that, outside a test, is never non-null. **The owner's game folder does
  not have this build, and does not need it.**

## Evidence

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/agent-replay`, target `PoseidonTests`,
clean. Built with the Bash tool and the SDK `mt.exe` on `PATH`, not `Build.ps1`.

| run | test cases | assertions | failures |
|---|---:|---:|---:|
| baseline (`48a26cff`, and unchanged after the rebase to `5ec56657`) | 3019 | 240,905 | 0 |
| after the timeline + its tests (`903debac`) | 3030 | 241,659 | 0 |
| after the box3d arm (`ff1efd8d`) | 3033 | 241,798 | 0 |

The difference is exactly the 14 new cases and 893 new assertions. 20 cases skip in every run —
external game data absent on this box.
