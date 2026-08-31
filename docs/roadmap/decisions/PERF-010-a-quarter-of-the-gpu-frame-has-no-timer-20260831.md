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

## The two hypotheses, neither yet tested

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
work.

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
