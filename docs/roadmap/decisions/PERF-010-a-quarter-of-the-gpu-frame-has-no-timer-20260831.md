# PERF-010 — A quarter of the GPU frame is attributed to nothing

**Date:** 2026-08-31
**Build:** `asset-obs-001` @ `c0211e7b`, exe and DLL deployed as a matched pair by `Deploy.ps1`
**Roadmap:** Phase 3 (measure the actual bottlenecks), and a precondition for track B.4

## The measurement

`perf_field.eden`, windowed 1600×900, screenshot delay 900 frames, one run per arm, same
binary, back to back. The capture's own `build{}` block records the binary and both
resolutions, so these numbers can be checked against the exe that produced them.

| Arm | upscaler | render → output | GPU frame | attributed by timer regions | **unattributed** |
|---|---|---|---:|---:|---:|
| `WGR_DLSS` unset | dlss (active) | 1072×603 → 1600×900 | 11.559 ms | 7.658 ms | **3.900 ms (34%)** |
| `WGR_DLSS=0` | bilinear | 1072×603 → 1600×900 | 10.443 ms | 7.607 ms | **2.835 ms (27%)** |

"Attributed" is the sum of every top-level row — the ones the capture's own contract
(`contained_by == -1`) says add up to the frame. The two arms render the same internal
pixel count, so this is a controlled comparison of the reconstruction step, not of scene
cost: attributed work differs by 0.05 ms between them, which is noise.

**Two things fall out.**

1. **DLSS costs about 1.07 ms here** (3.900 − 2.835), at 1072×603 → 1600×900 on an
   RTX 3070. That is the first time this repo has priced it. It is invisible to the
   profiler because the NGX evaluate is the vendor's own dispatches inside our frame
   bracket — we cannot bracket work we do not encode.
2. **2.8 ms — 27% of the frame — is unattributed with DLSS OFF.** That is ours, and
   nothing measures it.

## What it is not

Not missing features reporting stale values: 14 of the 58 regions read `-1`, and all 14
are legitimately-off passes (planar reflection, underwater, caustics, whitewater, far
tier). The `-1` convention is trustworthy as of today — see the GPU/CPU timer fixes
committed the same day.

Not a resolution artefact: both arms render 1072×603 internally. `WGR_DLSS=0` does **not**
give you native — it gives you **bilinear** upscaling of the same reduced render target,
which is worth knowing before quoting a "DLSS off" number as a native baseline.

## CORRECTION, same day: the denominator was wrong, and fixing it answered the question

Extending the measurement to four scenes exposed a bug in the measurement itself. On
`perf_water` the sum of outermost rows came to **20.485 ms against a 20.322 ms frame** — a
**negative residual**, the parts explaining more than the whole. A partition that
over-counts is not a partition, so the "27% unattributed" above could not be trusted as
stated.

Cause: `GpuTimerContainedBy` (and its `wgpu_renderer.hpp` mirror) did not list **`Water
draw`** or **`Post: god rays`** as being inside the 3D colour sub-pass. Both are: the
colour segment's bracket spans `lib.rs:4742-5134` and water draw sits at 4929-4970 with
god rays at 5071. They were missed because, unlike every other row in that list, they open
their **own render pass on the encoder** instead of being bracketed inside one, so they
read as standalone at the call site — but containment here is about the *timestamps*, not
the pass structure. God rays in particular was moved into that span by a recent change
("God rays composite before the transparent draws, not after everything") and the
containment table did not follow it.

Fixed, redeployed, re-measured from the binary — both rows now report `contained_by=41`:

| Scene | frame | attributed | residual | |
|---|---:|---:|---:|---|
| `perf_water` | 20.052 ms | 17.368 ms | **2.684 ms** | 13.4% |
| `perf_field` | 10.529 ms | 7.650 ms | **2.878 ms** | 27.3% |

**And that answers the question the section below was written to leave open.** The residual
is **near-constant in absolute terms** — 2.7 to 2.9 ms — across frames of 10.5 ms and
20.1 ms, i.e. across a doubling of scene cost. Across the four scenes measured before the
fix (corrected arithmetically) it is 2.86 / 3.04 / 3.80 / 2.77 ms. It does not scale with
work.

So it is **fixed per-frame overhead**, not un-bracketed scene work. A constant 3 ms is 13%
of a heavy frame and **27% of a light one** — it does not shrink as everything else is
optimised, so it grows as a share of every improvement made elsewhere.

## SECOND CORRECTION: "fewer, larger passes" was wrong, and here is the number that killed it

The paragraph above originally continued: *"pass setup and teardown, barriers,
render-target transitions across ~43-48 pass boundaries, which is ~65 µs each. The lever is
fewer, larger passes."* That arithmetic works (2.8 ms ÷ 45 ≈ 62 µs) and it is wrong. It was
a hypothesis stated as a conclusion, and the residual never actually correlated with the
region count — `combat` and `field` both ran 43 regions with residuals of 3.80 and 2.77 ms.

**Tested directly.** `hiz.rs` built its depth pyramid with **one compute pass per mip
level** — ~11 of them at 1072×603 internal. Merged into a single pass (legal: WebGPU's
compute usage scope is one *dispatch*, and dispatches within a pass run in order with
implementation-inserted barriers). Same pipeline, same bind groups, same dispatch sizes,
same order; ten pass boundaries removed. `perf_field`, DLSS off, 1600×900:

| | Hi-Z region | frame | residual |
|---|---:|---:|---:|
| before | 0.170 ms | 10.529 ms | 2.878 ms |
| after, run 1 | 0.125 ms | 10.726 ms | 3.090 ms |
| after, run 2 | 0.130 ms | 10.954 ms | 3.341 ms |
| after, run 3 | 0.128 ms | 10.697 ms | 3.112 ms |

**The Hi-Z region got 25% cheaper — a reproducible 0.042 ms — and the residual did not
move.** Which gives the number this whole section was missing: 0.042 ms across ten removed
boundaries is **~4 µs per compute pass boundary**, not 65. At that price the ~45 boundaries
in a frame cost about **0.18 ms — roughly 6% of the residual**, not all of it.

So the residual's cause is **still open**. What is now established:

- it is fixed per frame (2.7–3.3 ms across scenes costing 10 to 20 ms);
- it is **not** pass-boundary overhead, which is 20× too small to explain it;
- pass-count reduction is a real but small optimisation, worth taking where it is free
  (the Hi-Z merge is kept for that reason, not for a frame-level win it did not deliver —
  frame totals across these four runs, 10.53 to 10.95 ms, are noise).

Candidates that survive: swapchain acquire/present inside the frame bracket, and work the
GPU does between the last timestamp and the frame-total close. Neither has been measured.
**Do not act on this section's advice until one of them has.**

## The two hypotheses as originally written, kept for the record

- **Inter-pass overhead.** 44 regions ran this frame. Each region's timestamps bracket
  only its own work; barriers, render-target transitions, cache flushes and pipeline
  bubbles between consecutive passes belong to no region and are real GPU time. 2.8 ms
  over 44 pass boundaries is 64 µs each, which is the right order for this to be the whole
  story. **If this is it, the optimisation is fewer, larger passes** — and that is a very
  different piece of work from making any individual pass faster.
- **Genuinely un-bracketed work.** Swapchain acquire, the composite blit, clears, mip
  generation. Each is cheap alone; together they could account for a share.

Distinguishing them is cheap and nobody has done it: bracket the whole encoder in one
region and compare against the sum, or vary the pass count (turn off a feature that owns
several regions) and see whether the unattributed time moves with the count or with the
work. *(Done, above: it moves with neither — it is constant.)*

## Why this comes before track B.4

Grass is the largest *attributed* cost — 6.415 ms across three passes on `perf_abel`,
20.8% of a 30.853 ms frame (`Grass prepass` 1.903, `Grass colour` 3.445, `Grass shadow`
1.068). It is the obvious target, and B.4 exists for it.

But the unattributed block is larger than grass. Optimising the second-largest cost while
the largest is unmeasured is how a session spends a day for 2% — and this repo has a
standing rule about measuring denominators for exactly that reason. **Price the 27% first.**

## Reproducing

From the game directory, with `nvngx_dlss.dll` beside the exe (without it DLSS silently
falls back and the arms become identical):

```
OpenPoseidon.exe --check --render wgpu --window --width=1600 --height=900 \
  --test-mission <repo>/tests/perf/missions/perf_field.eden --test-type screenshot \
  --screenshot <png> --screenshot-delay 900 --capture-metrics <json> --log-file <log>
```

Then sum `gpu_timings_ms[]` rows with `contained_by == -1`, excluding `GPU frame total`,
and subtract from it. Set `WGR_DLSS=0` for the second arm.

An exit code of 139 (segmentation fault) after `Shutdown complete` on the DLSS arm is the
NGX teardown crash and is the **witness that DLSS actually initialised**; a clean exit on
that arm means it silently fell back and the comparison is worthless.
