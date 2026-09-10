# Parked — everything deliberately not being worked on

One page, so nothing is parked only in somebody's head. **Parked is not the same as
forgotten and not the same as abandoned**: each entry says who parked it, why, and what
would unpark it.

Ordered by what it would take to pick up again, cheapest first.

Last status reconciliation: 2026-09-10. Dated evidence below remains historical.

## Current owner authorisation, 2026-09-10

The owner explicitly reopened visual/performance improvements and authorised a
review of this list, small fixes before architectural work. This is not blanket
acceptance of previously rejected experiments. Current execution and acceptance
are tracked in [SEPTEMBER9-VISUAL-FOLLOWUP.md](SEPTEMBER9-VISUAL-FOLLOWUP.md).

- **Native Reforger load is implemented, not parked.** Native forest/material
  quality and performance are active again. Compare settled identical scenes;
  do not re-export the converted corpus as a substitute for repairing native load.
- **Render/simulation overlap is implemented.** The August render-thread blockers
  below record its development history, not the current gate. See PERF-018 and
  the September 1 roadmap evidence (901/901 matched frames). Later ownership fixes
  are still relevant; passing the old gate does not excuse new race regressions.
- **Arma 1/2 original mission compatibility is planning work, not a completed
  loader feature.** See [ARMA12-DIRECT-MISSIONS-20260910.md](ARMA12-DIRECT-MISSIONS-20260910.md).
- **GL33 retirement remains deferred.** The owner explicitly places it in the
  far future. Do not remove it as part of this renewed programme.
- **Rejected visual changes remain rejected** until new evidence justifies an
  isolated A/B. Audio listening judgements remain separate from graphics work.

The remaining historical entries still require individual code/evidence review;
this reconciliation does not claim that every parked item has been implemented.

Current evidence gates added September 10:

- **Late foliage atmospheric replay/surface-rays:** keep both experiments off.
  0b684058's extra per-surface integration failed the installed repeated hillside
  comparison and adds colour-pass cost. Reopen with a diagnosed discrepancy and
  equal-scene controls, not a global vegetation brightness boost.
- **Smoke cold-room generation is active and improved**, not an argument to
  lower smoke count: 8c9c4a87 reduces the tested fire-station cold build from
  31.5 to 4.3ms while preserving exact room/portal hashes. Other building shapes
  and full smoke-containment quality remain independent coverage.
- **Army performance:** the counted palette upload batching is accepted for
  its bounded setup saving. General BT/HTN replacement, broad AI multithreading
  and density/visibility cuts are not justified by this result.

---

## Waiting on the owner, five minutes each

| Item | What to do | Where |
|---|---|---|
| **HRTF audio modes** | Options → Audio, step the HRTF row Off → On → Auto. Each change must not glitch or stop audio that is already playing. Then grep the log for `HRTF:`. | `AUD-101` |
| **Mouse-wheel bindings** | Options → Controls → Keyboard & Mouse. Map Zoom In/Out should read "Mouse wheel down/up" by default, **including on an existing profile**. Rebind, swap the two, restart. | upstream backport `a9cdfc29` |

Neither blocks anything. Both are code that has never been exercised by a human.

---

## Parked by the owner, with a reason

### Night lit-ground artifacts under DLSS
**Parked 2026-08-31 by the owner** ("ist aber jetzt egal dass soll geparkt werden"), while
confirming the soldier bug fixed. A separate defect from that one; re-confirmed present on
the fixed build. Background-task chip `task_c060eaf7`.

### Reforger Everon re-export (the white blocks)
**Parked 2026-08-31 by the owner**: roadmap has priority, and this is hours of work.
The *converter* fix is done and measured — colour-only `.emat` materials went from 0 to 11
deployed on a 400-model export (MAT-054) — but the deployed 8.4 GB corpus is still the one
built by the old converter, so **the blocks stay white until a re-export runs**. Unpark by
saying so; it is a multi-hour unattended job writing into the game data.

### Loading Reforger's Everon natively, without conversion
**Parked 2026-08-31 by the owner**: after the roadmap. Investigated rather than guessed —
six of the eight formats already have engine-side readers, two of them (`.edds`, `.emat`)
are already wired into the runtime, and exactly one reader (the `.et` prefab graph, ~100
lines) is missing engine-side. Plan and sizing in `RFG-002`; steps 1 and 2 are independent
and carry no runtime risk when someone picks it up.
**Un-parked 2026-09-01 by the owner** ("reforger Karte muss geladen werden können ohne
Konvertierung"): the native loader is live work on `asset-obs-001` (RFG-0xx series in the
code -- roads, rotation, doors/windows, clutter, layer tint, church walls, café Things). The
converted map is a source of solved sub-problems, not the quality bar.

### Reforger's blade layer as its own geometry
**Parked 2026-09-06.** A `PlantMat` is `Plants 8` x `BladesCount 3` crossed CARDS of a whole
tuft cut from its atlas, 10 cm tall. RFG-089..091 put that atlas on our near-ring blade
strips (tuft stretched onto the thin quad, ground-tinted by the surface's mean albedo in
place of the satellite map, root shade and grading chain bypassed), which reads right at
eye height but is not the geometry. The faithful port is the card path at blade spacing
with the PlantMat's `DistributionAtlasUVs` as the card atlas and `Height` as the card size --
and the satellite pages bound for the tint (`SatMapLerp 0.8`), which the native terrain
does not have either. Unparks together with the supertexture pages.

### EFX reverb environments
**Parked 2026-08-30 by the owner** after an A/B: *"klingt nicht richtig und nicht gut"*.
Behind `POSEIDON_EXPERIMENTAL_EAX=1`, default off. One concrete defect was found inside it
later and fixed (interiors ran at a modal density of 0.02–0.08 against the 0.4287 the ROOM
preset ships with) — that is *one checkable thing that was wrong*, not an answer to the
verdict. Unparking means listening again, with that fix in. `AUD-102`.

---

### The pale, speckled Arma 3 bushes — diagnosed, not fixed
Owner-reported on Stratis as "flimmert das Gras", then narrowed by them to *"visible in a
single frame, this plant type only, other grasses work"* — which was the observation that
cracked it. It is **`a3\plants_fush_neriumo2d_f.p3d`, the DEAD nerium oleander**,
named three independent ways including a world-free `--test-model` scene standing it beside
the LIVING oleander, which renders correctly in the same frame.

**The mechanism is geometry, not materials.** The working oleander's LOD0 is a leaf atlas
plus an opaque bark section; the dead one puts the same alpha atlas on *both* sections, so
it has no solid geometry anywhere and every pixel rides the Blend→cutout promotion. That
promotion is measured and then *not applied* (`AlphaMaskDemoteEnabled()` gates it off), so a
plant made entirely of sub-pixel twigs is left to partial coverage over bright sky — pale
body, threshold-flipping speckle, flicker.

The main session's hypothesis (a normal map bound as albedo) is **dead**: GL33, which
applies no cutout at all, renders this bush the correct dark brown of its atlas, and
suppressing every production material binding leaves the pallor untouched. **Unparks with
one log line at the end of the alpha ladder**, diffed between the two atlases — that decides
between the two remaining explanations. `MAT-055`.

## Parked on evidence — the premise did not survive measurement

### GPU-cost clamp on the object item budget
`WGR_OBJECT_STREAM_GPU_BUDGET=1`, **opt-in**. Correct where it matters (zero interventions
at the probed 5,611 MB budget, so a set that fits is never shrunk) and wrong under
pressure: at 1,200 MB it clamps 20,000 → 13,608 and then changes its mind **96 times in
200 s**, because shrinking the set changes the per-object measurement that drove the
shrink. Needs a dwell time on the decision, a windowed cost average, and a measurement of
what the release path does when the desired set shrinks. All three are cheap.

### Retained-texture liveness pin exemption
`WGR_RETAINED_LIVENESS=1`, **opt-in**. Built to cure white buildings under memory pressure;
the white buildings turned out not to be an eviction symptom at all (`REN-RES-001`), so
relaxing a safety check buys nothing measurable.

### "Fewer, larger passes" as a performance lever — **withdrawn**
Proposed and retracted the same morning. A compute pass boundary costs **~4 µs** on this
hardware, not the ~65 µs the arithmetic suggested: merging Hi-Z's eleven per-mip passes
into one made that region 25% cheaper (0.042 ms) and left the frame residual untouched.
Do not spend effort merging passes on the strength of that number. `PERF-010`.

### Grass work as a whole — closed for now
4.882 → 2.604 ms, frame 13.34 → 10.5 ms. Every lever B.4 listed is resolved: vertex count
taken, prepass eliminated, cascade mask already existed, cards characterised and parked
below. **Reopen with a corpus that measures differently, not with another idea.**

### Grass photo cards as a cheaper meadow
34× cheaper than the procedural blades (frame 13.34 → 7.62 ms) and **not a replacement**:
the atlas is authored as large sparse clumps, so the default spacing gives bare ground with
isolated tufts and densifying gives oversized reeds. Spacing and card size are coupled and
only spacing has a slider. **Unparks with re-authored smaller cards** — a content job with a
3–5 ms prize. The owner's earlier "flickering, far too dense" verdict predates both fixes
that were made to the card path.

### The grass depth prepass — **a wash, measured 2026-08-31**
Costs 1.449 ms and saves 1.353 ms of grass colour; net zero. Turning the prepass off drops
the frame 1.3 ms only because ambient occlusion goes with it, which is a feature trade, not
a win. The vertex-count lever it pointed at **has since been taken**: indexing the blade
ribbons cut grass from 4.882 to 2.973 ms (see B.4 in the roadmap), which is why this entry
is about the prepass only.

---

## Found and not fixed — defects the ownership audit turned up

Recorded here because they are real, none is urgent, and none is being worked on. Full
detail in `SIM-805`; the table itself is `docs/roadmap/ownership-authority.md`, now 36 rows
with a **Rule** column saying whether each is *stated*, *implicit* (obeyed by accident) or
*checked* (the program notices a violation). **Four are checked. That ratio is the number
to watch.**

### The savegame version is written and never compared
**Closed for future-version rejection, 2026-09-10, a23c43de/883f174a.**
World::Serialize checks the maximum supported archive version before any world
mutation. Old/current versions retain existing per-field defaults. Native
old/current/future two-pass tests and an installed current-version save/load
round-trip pass; see SEPTEMBER9-VISUAL-FOLLOWUP.md. This does not establish a
complete migration/identity contract for every historical save. Original finding:

`WorldSerializeVersion = 13` appears in exactly three places and **all three are writes**.
The reader's only failure mode is the field being absent; any value that parses is accepted,
including one from the future. Its per-field gating (`if (_version < minVersion) return
LSOK`) is right for an older save and exactly backwards for a newer one. Roadmap 4.1's
closing requirement is unmet.

### Multiplayer authority is a bool checked by convention at ~150 sites
Every mutator is public and callable on a remote object; the nearest chokepoint only
refuses to *send* an update it already applied locally. `Object::IsLocal()` hardcodes
`true`.

### `BACKGROUND_AI` gates a class that does not exist
`SecondaryThread` is forward-declared at `Core/Types.hpp:36` and **never defined**. The gate
is off by an explicit `0` in two files and by an *undefined macro* in `World.cpp`.
Uncompilable dead code, and the reason SIM-804 could establish "nothing touches physics from
another thread" so easily.

### A static object's `NetworkId` derives from an id that `ResetIDs` renumbers on save
4.1's "no ephemeral handle becomes a persisted identity" is clean by grep — but this is its
mirror image and was not.

### 274 `RptF` diagnostics that compile to nothing
`#define RptF(...) ((void)0)` (`DebugLog.hpp:61`), unconditional, in every build. **274 call
sites** across the engine, all silent. One of them was the sole-renderer gate's cited
evidence for "an intentional WGPU init failure produces a clear error" — the refusal
behaved correctly and said nothing, and the gate row had been written by reading the source
as if it were behaviour. That one is now a real `LOG_ERROR`. **The other 273 are not
touched**, deliberately: they are legacy 2001 diagnostics and turning them all on at once is
a log-volume change nobody has measured. Unpark by measuring the volume on a traverse first,
then deciding per subsystem.

### The object-stream upload cap: works, unknown whether it helps
`WGR_OBJECT_STREAM_UPLOAD_MB`, default off. The new `admit_stop_upload` counter proves it
fires — 17 to 19 times per Everon traverse, stable to ±1. Whether turning it on helps is
**not** settled: resident objects came out 68k–101k with it off against 76k–119k with it on
over three runs each, so the 12k gap between the means is indistinguishable from noise on
that scene. Unparks with a quieter measurement, not with another traverse.

## Parked on dependencies — blocked by real work, not by a decision

### Phase 5 step 6 — the render thread
Four named blockers in `render-snapshot-design.md` §3b. Two of the original six are now
closed (the dangling `Helicopter*` and the renderer mutating streaming state mid-draw), and
**blocker 3 (stable render-object identity) is reduced to one scheme of three** — the other
two were already fixed on 2026-08-30 and the design doc simply had not been revised. What
is left there is `Object*` map keys, and the roadmap overstates it: there is no
use-after-free today, and since the instance handles gained generations the worst outcome
of a recycled address is a leak. A separate, real defect surfaced in the audit: the
motion-vector id folds to a **16-bit lane**, so two live objects 65,535 ids apart share one
history slot. `REN-ID-001`.

**The single-buffered push path is no longer the blocker — it was closed on 2026-08-31**
(`84a44e79`). The fifteen per-frame arrays plus `cameras`, which was a local and would have
been *gone* rather than merely overwritten, now swap into a two-slot ring at publication.
Verified by identical counts across three runs per arm (98,684 registered / 452 main /
25,992 tris either way). That also closed blocker 3 by placement rather than by locking: the
two readers of `_gpuInstances` both sit producer-side of `wgr_render_frame`, so the renderer
never touches the live map and no mutex was needed.

**What blocks it now is two things, both measured the same day and both larger.**

*The handle is exclusive.* 62 of 79 `wgr_*` entry points form `&mut *renderer`, so two
threads in any two of them alias an exclusive reference — UB in Rust's model, invisible to
wgpu's validation. Only about ten call sites are actually in the way (the ones the producer
makes while a frame renders), and they collapse to one mechanism: a queue drained at one
point per frame. Creates cannot merely move threads, because `mesh_create` clears a bind
cache the draw path reads. `REN-THR-001` has the counts, the call-site table and the
per-frame traffic — which is **zero on Abel and 94–513 instance adds per frame on Everon**,
so the bench scene is precisely the one that would have sized the queue wrong.

*And there is no way to prove such a restructuring is equivalent.* Phase 5 step 6 requires
it. The instrument now exists — the published `WgrFrame` is hashed at the publish point
(`WGR_FRAME_HASH=1`) and is not vacuous — but three back-to-back runs of one scene agree for
only ~24 frames past the load and then diverge absorbingly: by frame 800 they share not one
frame hash. Pinning the simulation's `deltaT` does not help (24 → 19), nor does forcing the
object stream to a fixed synchronous batch. `REN-THR-002`. **Finding what makes two runs of
the same scene publish different frames is now the gating item**, ahead of the queue, since
the queue is the first change that would need proving.

### GL33 retirement
Evidence gathered, recommendation is **do not retire**: CI runs 111 of 117 Trident scenarios
on GL33 and **zero** on WGPU, so deleting it today deletes the only backend the automated
suite can run. Owner decision, not an agent's. `REN-GL33-001`.

### Phase 6 slice 3 — material interpretation on workers
**Do not attempt without a corpus that measures differently.** `swap` is 27.171 ms of a
28.686 ms frame; all main-thread CPU work sums to ~1.5 ms and material interpretation is a
fraction of the 0.081 ms `land:obj` bucket. There is nothing to reclaim on a stock world.

### Renderer-bug triage without GL33 — gate bullet 8, still PARTIAL
The written procedure now exists (`docs/roadmap/renderer-bug-triage.md`) and was scored
against the three historical cases rather than the ones it was written for: `RND-031` is
reachable without GL33 at all, `MAT-030:146` never needed it (GL33 drew that birch wrong the
same way), and **`WLD-021` is genuinely lost** — no Takistan in the archive, and GL33 never
got compat coverage so one cannot be added. Two harness facts worth knowing: `lint.yml`
gates the machinery, not any archived scene; and `changed_pixel_ratio` cannot rank severity
— a blacked-out object scores 0.027 while a signed-off, non-blocking backend difference on
the same scene scores 0.55. **Unparks when bullet 6 covers a non-CWA family.**

### Order-dependent `DDSConverter` test
Passes in the full suite, fails when run alone (`32 == 64`). Relies on state an earlier test
leaves behind. Chip `task_cc1f2932`. Do not "fix" it by accepting 32 — work out which value
is correct first, because a converter silently producing a half-size level is the more
interesting possibility.

### Loose objects read a solver that has not stepped yet
**Parked 2026-08-31 by SIM-814**, which found it while auditing the stage graph's resource
declarations and is deliberately not fixing it in the same change. The Vehicles stage reads
a body's pose — `world->GetBodyMotion(body, motion)`,
`engine/Poseidon/World/Physics/LooseObjects.cpp:100`, reached from `Thing::Simulate` — and
the Physics stage steps the solver afterwards, `engine/Poseidon/World/World.cpp:2018`. So a
loose object's position is always one tick behind the solve that produced it. The graph now
reports it as the previous-tick edge `physics → vehicles (physicsState)`, and
`test_sim_stage_evidence.cpp` pins it, so it cannot vanish unnoticed.
**Unparks** when somebody decides whether to move the hand-over after the physics step or
the physics step before the vehicle stage — a physics-behaviour change with no test
coverage today, and `LooseObjects` is off by default (`PHY-010`), so nothing visible is
waiting on it.

### A flare runs a script from inside the vehicle stage
**Parked 2026-08-31 by SIM-814.** `IlluminatingShell::Simulate`
(`engine/Poseidon/World/Entities/Weapons/Shots.cpp:1642`) calls `GWorld->AddScript(script)`
and then `GWorld->SimulateScripts()` at `Shots.cpp:1665-1667`, so the Scripts stage
re-enters in the middle of the Vehicles stage. Harmless while everything is serial, and
rare — it needs a flare *and* an `onFlare.sqs` in the mission. It matters for Phase 8: under
concurrency a script would run on whichever thread happened to be simulating a flare, and
"a stage is a stage" is the assumption the whole task graph rests on. The declaration now
records it (Vehicles reads and writes `ScriptState`) rather than pretending otherwise.
**Unparks** with Phase 8's deterministic command-and-commit stage, which is where
"a stage may not re-enter another stage" becomes enforceable rather than aspirational.

---

### Three ledger records whose production call sites do not resolve
`RND-033`, `MAT-047` and `RND-034` name symbols that are not in the files they cite —
`WaterSim::update`, `EngineWgpu::LogObjectStats`, `PAAEncoder::WritePAA`,
`ApplyTestWorldFreeFly`. All three carried `status: PARTIAL`, which is not one of the eight
lifecycle states and never has been, so the ledger gate rejected them the moment it could
see them at all. They were set to `RESEARCHED` on 2026-08-31 — deliberately the conservative
reading, since each has an empty `implementation_commits` list and a decision record that
does support a finding.

**What is not known is whether a fix landed for any of them and simply was not recorded.**
That happened to `CLD-021` in the same batch: its `select_cloud_shadow_span` does exist
(`sky/mod.rs:1980`, added by `bd00297d`), so its record had been understating shipped work,
and it was corrected to `INTEGRATED`. Unparks by doing for these three what was done for
that one: find the symbol, find the commit, promote or leave it. Half an hour each, and it
must not be guessed — `RESEARCHED` under-claims safely, `INTEGRATED` does not.

---

## How to use this page

If you park something, add it here with the reason and the unpark condition. If you unpark
something, delete its entry in the same commit as the work — an entry that is quietly wrong
is worse than no page.

## Native Everon: forest contrast and the church doors (parked 2026-09-06)

**Forest reopened by the owner, 2026-09-10.** The historical lighting-only
diagnosis below was incomplete: native `_ntc` textures were not classified as
RG normals, so the retained shader read cavity alpha as normal X. b4d118ce /
3f736b26 repair compressed flags and decoded normal repacking, with 210 targeted
assertions and installed same-binary Forest A/B. Crown directional relief improves
without extra samples; branch-card geometry, self-shadow/AO and exact Reforger
reference matching remain open. See SEPTEMBER9-VISUAL-FOLLOWUP.md for captures.
Do not apply a global contrast adjustment as a substitute for material fidelity.

- **Forest looks flat.** The owner's forest pose (4697.60 3998.02 50.23 az 220.6 el -5.0)
  shows pines with little tonal range. Ruled out so far: the layer tint (a LINEAR
  multiplier, applied correctly), the canopy flags (native trees carry `map=tree/bush`, so
  the foliage lighting runs), UV decode (RFG-102 fixed the buildings; polyplanes were never
  striped). What is left is lighting: the polyplane composites get no self-shadow / AO and
  the foliage wrap flattens them. Needs a side-by-side with Reforger at the same pose and
  hour before tuning anything.
- **Church doors "inside the building".** RFG-100 measured the door sockets: the big
  doors sit at the shell origin, which is the doorway between porch and nave; the owner's
  pose is in the porch. They are placed correctly and CLOSED. Reforger opens them by
  interaction; a default-open state for `Door_*` parts would need the door prefab's open
  angle, which the loader does not read.

