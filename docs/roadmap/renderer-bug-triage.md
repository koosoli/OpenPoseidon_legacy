# Renderer bug triage without launching GL33

This is gate bullet 8 of the WGPU sole-renderer gate
([REN-GL33-001](decisions/REN-GL33-001-sole-renderer-gate-evidence-20260831.md)): *"a
renderer-bug triage story exists that does not require launching GL33."* A renderer bug
is reported. Here is what you do, in order — and, just as importantly, here is how you
recognise the answer "the archive cannot decide this" instead of manufacturing one.

Written 2026-08-31 against `agent-gl33-triage` @ `3c7025ec`. Every number below was
measured by running the harness on the committed archive, not estimated.

---

## 0. What you are working with

**The harness.** `scripts/compare_preview0_captures.py` takes two PNGs of identical
dimensions, produces a 4x-brightened difference mask and a JSON report, and optionally
applies an acceptance profile. It reports:

| field | meaning |
|---|---|
| `changed_pixel_ratio` | fraction of pixels whose mean absolute RGB delta exceeds `--threshold` (default 12) |
| `mean_absolute_rgb_delta` | mean over every pixel |
| `max_absolute_rgb_delta` | worst single pixel |
| `regions` | per-tile ratios on a `--regions ROWSxCOLS` grid (default `4x4`), plus `peak` |
| `acceptance` | only when `--acceptance-profile` is passed |

It does **not** align, warp, ignore, or tolerate anything. There is no per-region
masking-out, no perceptual metric, no structural similarity. It is a raw per-pixel
delta with one global threshold. Treat it accordingly.

**The acceptance profiles.** Three policies: `thresholds` (numeric pass/fail),
`review-required` (always `REVIEW_REQUIRED`), and `correctness-review` (accepts only if
a named reviewer signed off *these two exact SHA-256s* with a classification listed in
`classification.non_blocking`). The one committed profile,
`visual-acceptance-profile.preview0-original-training.json`, is `correctness-review`.

**What `lint.yml` actually gates.** Read this carefully, because it is easy to
misread. The "Validate explicit visual-review acceptance gate" step does **not** check
any capture for drift. It runs the harness twice over the same fixed pair and asserts
that the *machinery* works: once with the committed profile (must accept), then once
with the profile's classification rewritten in-memory to `CORRECTNESS_BUG` (must
reject). It is a self-test of the review gate. **No archived scene is currently gated
against regression by CI.** Do not assume a red CI would have caught a pixel change.

**The archive.** `docs/roadmap/evidence/gl33-freeze/` — see its README for provenance.
As of `3c7025ec`:

| Scene | GL33 | WGPU | Pairable |
|---|---|---|---|
| `everon-day` (`devtest.abel`) | yes (+ `.log`) | **no** | no |
| `everon-night` (`devtest-night.abel`) | yes (+ `.log`) | **no** | no |
| `town` (`perf_town.noe`) | yes | yes | yes |
| `water` (`perf_water.eden`) | yes | yes | yes |
| `combat` (`perf_combat.eden`) | yes | yes | yes |
| `field` (`perf_field.eden`) | yes | yes | yes |

All 1600×900. Four pairable scenes, all stock CWA. Two known archive defects: the
README's table promises a `.log` beside each of the four new scenes and **none was
committed**, and its provenance block names two different commits (`99ac0f91` in the
appended section, `92ebd1d8` in the bullet below it) for the same captures.

---

## 1. Calibrate before you interpret

The single most important thing to know before reading a number out of this harness is
that **`changed_pixel_ratio` has no absolute scale.** Measured, on the committed
archive:

| Comparison | `changed_pixel_ratio` | `mean_absolute_rgb_delta` |
|---|---:|---:|
| A file against itself (the floor) | 0.00000 | 0.00 |
| **A 200×200 object blacked out, same backend, same scene** | **0.02733** | **0.98** |
| GL33 vs WGPU, `town` | 0.5499 | 17.73 |
| GL33 vs WGPU, `water` | 0.8630 | 28.08 |
| GL33 vs WGPU, `combat` | 0.9167 | 41.53 |
| GL33 vs WGPU, `field` | 0.9086 | 58.36 |
| GL33 vs WGPU, `preview0-original-training` (the `lint.yml` pair) | 0.8855 | 44.51 |
| GL33 *day* vs GL33 *night*, same camera (an unrelated image) | 0.9951 | 114.78 |

Read row 2 against rows 3–7. **A whole object vanishing scores twenty times lower than
an expected, signed-off, non-blocking backend difference.** Anyone ranking bugs by
`changed_pixel_ratio` will investigate the non-bug and dismiss the regression. Three
consequences, and they are the spine of everything below:

1. **Never apply a `thresholds` profile across backends.** There is no threshold that
   separates GL33-vs-WGPU from GL33-vs-a-different-scene: 0.55–0.92 versus 0.995. This
   is not a tuning problem, it is the reason `TEST-002` decided the ratio is "retained
   as evidence, not a numerical release gate" and the reason the one committed profile
   is `correctness-review` rather than `thresholds`.
2. **The only comparison with a meaningful floor is same-backend, same-scene, same
   resolution.** There, 0.00000 is exact and any non-zero digit is signal.
3. **Magnitude is the wrong axis; concentration is the right one.** The blackout above
   put 0.248 and 0.189 into two adjacent tiles and **0.000 into the other fourteen**.
   Every cross-backend pair puts 0.3–1.0 into all sixteen. A defect is local; a
   capability difference is everywhere. That is why `regions` exists, and it is the
   field to read first.

---

## 2. The procedure

### Step 1 — Establish the binary before you establish anything else

CLAUDE.md rule 0. Open `<game folder>\DEPLOYED-FROM.txt`, confirm the exe and
`wgpu_renderer.dll` file times match, and record the commit. **A stale DLL is
indistinguishable from a fresh rendering regression**, and this harness will happily
quantify one for you to four decimal places. Put the commit next to every number you
report.

### Step 2 — Ask which of four questions you actually have

Every renderer bug report is one of these, and they take different routes. Deciding
this first is the whole method; almost every wasted triage session is one question
answered with another question's tool.

| # | Question | Route |
|---|---|---|
| Q1 | **Did we regress this?** "It looked right last week." | Step 3 — archive diff |
| Q2 | **Is this drawn at all?** "The trails/lines/decals are missing." | Step 4 — ablation diff |
| Q3 | **Is this the data or the renderer?** "The roads band on Takistan." | Step 5 — and the archive **cannot** answer it |
| Q4 | **Is this correct?** "The birch canopy looks thin." | Step 6 — no capture diff answers this |

### Step 3 — Q1: the archive diff (same backend, same scene)

This is the one thing the archive does well and the one thing it is for.

1. The scene must be one of the four pairable ones, and the bug must be visible in it.
   If it is not, stop and go to Step 7.
2. Re-capture on **WGPU**, same mission, same camera, `--window --width=1600
   --height=900`, exactly as the freeze README documents. `--window` is not optional:
   without it mission mode renders at display resolution and the harness refuses the
   pair outright ("capture dimensions differ"), which is at least a loud failure.
3. Diff against the archived **`wgpu-<scene>.png`**, never `gl33-<scene>.png`:

   ```
   python scripts/compare_preview0_captures.py \
     --reference docs/roadmap/evidence/gl33-freeze/wgpu-town.png \
     --candidate <your-capture>.png \
     --regions 8x8 --mask out/mask.png --out out/report.json
   ```

4. Read `regions.peak` and the tile grid **before** the global scalars.
   - **All tiles 0.000** — the scene is bit-identical to the archive. Whatever was
     reported is not in this scene at this camera. That is a real, reportable answer.
   - **A few tiles hot, the rest 0.000** — a localised change. `peak.box` is where to
     look. This is the shape of a regression.
   - **Every tile in the 0.1–1.0 band** — something global moved: exposure, tonemap,
     sun angle, time of day, or you are comparing across backends by mistake. Check
     the mission and the backend before concluding anything.
5. **Confound you must state, not assume away.** The archive is one commit
   (see §0). Any renderer change landed since then also shows up in this diff. A
   non-zero result means "different from the archive", *not* "regressed". Bisect
   against the archive commit before attributing it to the reported bug.

### Step 4 — Q2: the ablation diff (same backend, same scene, feature toggled)

For "is it drawn at all", you do not need GL33 and you do not need the archive. Capture
twice on WGPU from the same camera with the suspect path on and off, and diff the two.

- **`changed_pixel_ratio` exactly 0.00000 is a proof of inertness.** The toggle reached
  no pixel. That is a much stronger statement than "I can't see it", and it is the
  finding that closes this class of bug.
- Any non-zero, however small, means the path draws and the question becomes *what* it
  draws — which is Q4.

Where the toggles are: `WGR_ABLATE` takes a comma-separated list (`grass`, `ao`,
`skyvis`, `planar`, `clouds`) and **shouts on an unknown token** rather than silently
ablating nothing. Beyond it there are 273 distinct `WGR_*` environment names across
`engine/` and `apps/`.

**The trap, documented in `engine/WgpuRenderer/rust/src/lib.rs:307-309`:** several
renderer-side `WGR_*` gates — `WGR_GRASS`, `WGR_SKY_VIS` — are **inert at runtime**,
because C++ rebuilds and pushes the settings struct every frame and clobbers them.
`WGR_ABLATE` exists precisely because of this. So before you trust an ablation, prove
the toggle moved *something*: an ablation that reads 0.00000 because the env var was
ignored looks exactly like an ablation that reads 0.00000 because the feature is dead.
Toggle it in a scene where it is unmistakably visible first, confirm a non-zero diff
there, then take it to the scene under investigation.

`RND-031` is this route's worked example: six diagnostics drew nothing on WGPU because
one virtual was never overridden. An on/off pair scoring 0.00000 in a scene where the
trails should be blatant is that finding, in one comparison, with no GL33.

### Step 5 — Q3: the data-versus-renderer question

**Say plainly that the archive cannot answer this.** It is the one thing GL33 uniquely
did, and nothing in this document replaces it. The archive is stock CWA only — GL33
never received the compat-layer work — so for anything from Arma 1/2/3, DayZ or
Reforger there is no second witness in the tree at all.

What you do instead, in ascending cost:

1. **Ablate the renderer path and see if the artefact survives.** If a suspected
   shading term is disabled and the pattern is unchanged, that term is excluded. This
   is a same-backend diff and Step 4's trap applies.
2. **Read the data directly.** Decode the WRP placements, the model vertex normals, the
   RVMAT — do not infer them from pixels. This is slower than a paired capture and it
   is strictly stronger, because it excludes a hypothesis by measurement rather than by
   correlation.
3. **Probe pixels, not images.** Sample fixed points across a varying parameter (sun
   angle, time of day) and tabulate. A surface whose value does not track the sun has a
   structurally absent sun term; the harness's whole-image scalars cannot see this and
   `regions` only narrows it to a tile.

`WLD-021` (Takistan road banding) is the honest test case and the honest failure. Its
decisive line is a GL33 capture on identical data, and **the archive would not have
produced it**: no Takistan scene, no compat coverage, and GL33 could not have rendered
it faithfully anyway. But note what actually closed the ticket — seven hypotheses each
killed by routes 1–3 above, including a `WGR_DECAL_SCALE=0` ablation, a census of all
438,906 placement basis determinants, a decode of LOD0 vertex normals out of
`roads_e.pbo`, and exactly the three-sun-angle pixel table of route 3. The GL33 capture
was the shortcut that pruned the data hypotheses in one step. It was not the proof. The
replacement is real, and it is slower.

### Step 6 — Q4: "is this correct?"

**No capture diff answers a correctness question, and a second renderer is not an
oracle for one.** This is the recorded failure mode, and it is why bullet 8 does not
demand a perfect replacement.

`MAT-030-treeadv-shader-family-20260809.md:146` is the case: asked whether the birch
canopy was thinning wrongly, GL33 "draws the birch as one opaque dark mass with no
cutout applied at all, which is a worse answer than the one being questioned." Had a
GL33 birch been in the archive, the archive would have been wrong in exactly the same
way. Losing GL33 costs nothing here.

What that ticket used instead is the pattern to copy: an A/B with
`WGR_MATERIAL_DISABLE_NORMAL` set and unset came out **identical**, which excluded the
normal mapping as the cause without needing any second backend. Then the question was
reduced to a specific, checkable mechanism — mip-averaging a 1-bit cutout whose Stage0
is 87% transparent — and named as unresolved rather than closed. Prefer "excluded by
ablation, mechanism named, still open" over "the other renderer disagrees."

### Step 7 — When the archive cannot decide it, and what to do then

Stop and say so, in these cases:

- **The scene is not in the archive** — anything not stock CWA (all Arma 1/2/3, DayZ,
  Reforger content), any world, camera, weather or time of day outside the six entries,
  and any question about `everon-day`/`everon-night` that needs a WGPU side, because
  those two have no WGPU counterpart.
- **The bug is not visible at 1600×900** in a still frame — anything temporal
  (flicker, shimmer, TAA/DLSS ghosting, streaming pop-in), anything sub-pixel, anything
  that only shows in motion. A still-frame diff of a flickering artefact is a coin flip.
- **The question is Q3 or Q4** (§Steps 5 and 6).
- **The archive itself is suspect.** `REN-GL33-001` records that GL33 has not been
  re-validated since the freeze captures; run R3 from that document before leaning on a
  GL33 archive frame as ground truth.

Escalate to, in order of cost: a targeted ablation A/B (Step 4); reading the asset data
directly (Step 5.2); a pixel-probe sweep across a parameter (Step 5.3); a GPU capture
(RenderDoc/PIX) for anything about draw order, state or depth; and finally the owner,
for a genuine correctness judgement — which is what `correctness-review` in the
acceptance profile is for, and it requires a named reviewer against two exact SHA-256s
on purpose.

**Appending to the archive is a legitimate outcome of triage.** If a class of bug keeps
landing outside it, the fix is a new scene, captured on WGPU at 1600×900 with a
recorded mission and commit, not a looser threshold. Everything already in
`gl33-freeze/` is immutable once committed.

---

## Scorecard: the three historical cases

Honest answers, since a triage story that only works on the cases it was written for is
not one.

| Case | What GL33 did | Would this reach the same conclusion? |
|---|---|---|
| `WLD-021` — Takistan road banding | A paired capture excluded the data in one step | **No.** No Takistan in the archive, and GL33 has no compat coverage to add one. The ticket's own seven ablations and data censuses do reach it, by Step 5, more slowly. This is the genuine loss. |
| `RND-031` — six dead diagnostics | Worked under GL33, so the gap was visible | **Yes, and without the archive.** Step 4's on/off ablation reading exactly 0.00000 is a stronger statement than "GL33 draws it", because it names the submission path rather than the backend. |
| `MAT-030:146` — birch canopy | Gave a **wrong** answer (opaque mass, no cutout) | **Yes — by not asking it.** Step 6 routes correctness questions away from cross-renderer comparison entirely. The archive would have reproduced GL33's wrong answer; the ablation the ticket actually used needs no second backend. |

One of three, and the one that fails is the one nothing here claims to replace.

## What this document does not establish

- **That the archive is still accurate.** No game was launched for this document —
  another agent held the single allowed instance. Every number above comes from
  re-running the harness over committed PNGs. Whether a capture taken today matches
  `wgpu-town.png` is `REN-GL33-001` R3 and is open.
- **That four scenes are enough.** They are four scenes on one world family at one
  resolution, one time of day each. Step 7's first bullet will fire often.
- **That the region grid catches every localised defect.** A thin regression spread
  across many tiles — a wire, a horizon line, a shadow edge — dilutes in every tile and
  will read as small everywhere. `regions` distinguishes *compact* from *global*, which
  is not the same as *defect* from *not-defect*.
