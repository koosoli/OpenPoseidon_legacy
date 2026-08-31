# Parked — everything deliberately not being worked on

One page, so nothing is parked only in somebody's head. **Parked is not the same as
forgotten and not the same as abandoned**: each entry says who parked it, why, and what
would unpark it.

Ordered by what it would take to pick up again, cheapest first.

Last updated 2026-08-31.

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

### EFX reverb environments
**Parked 2026-08-30 by the owner** after an A/B: *"klingt nicht richtig und nicht gut"*.
Behind `POSEIDON_EXPERIMENTAL_EAX=1`, default off. One concrete defect was found inside it
later and fixed (interiors ran at a modal density of 0.02–0.08 against the 0.4287 the ROOM
preset ships with) — that is *one checkable thing that was wrong*, not an answer to the
verdict. Unparking means listening again, with that fix in. `AUD-102`.

---

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
a win. The grass lever is the **15.5 M vertices per frame** (10.5 M near + 5.0 M mid, drawn
three times), not the prepass.

---

## Parked on dependencies — blocked by real work, not by a decision

### Phase 5 step 6 — the render thread
Four named blockers in `render-snapshot-design.md` §3b. Two of the original six are now
closed (the dangling `Helicopter*` and the renderer mutating streaming state mid-draw). The
largest remaining is that the **push path is still single-buffered**: `_draws3d`, `_verts`,
`_cmds`, `_palette`, `_cameras`, `_shadowCasters` are cleared in `InitDraw` and appended to
by the world traversal on the same thread. Deliberately **not** built ahead of its
consumer — doubling that storage before a render thread exists buys nothing and costs
memory.

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

---

## How to use this page

If you park something, add it here with the reason and the unpark condition. If you unpark
something, delete its entry in the same commit as the work — an entry that is quietly wrong
is worse than no page.
