# PERF-011 — What a day of renderer work is actually worth, end to end

**Date:** 2026-08-31
**Asked for:** a whole-frame before/after, not a collection of individual timer deltas.

That distinction is the point of this document. The day's commits each quoted a saved
region — 2.55 ms here, 39% there — and a sum of region deltas is not a frame time. This is
the frame time.

## Method

Same scene, same settings, same machine, **five runs per arm**, both arms forced to the
same upscaler so the comparison is of the renderer and nothing else.

- `tests/perf/missions/perf_abel.abel`, windowed **1600×900**, screenshot delay 900 frames.
- Internal render resolution **1072×603** in both arms, MSAA 4, upscaler **bilinear**
  (`dlss_active = false` in both — see the DLSS section below for why that is the honest
  arm and not a limitation).
- Arm A0 = `93a6433f`, built and deployed from its own worktree. Arm B = `6291292e`.
  57 commits apart. Both stamped into the game folder by `Deploy.ps1`; A0 needed `-Force`,
  which is the guard correctly refusing an ancestor deploy.
- Every number below comes from `--capture-metrics`, whose `build{}` block records the
  binary's version and both resolutions — so each figure can be checked against the exe
  that produced it.

## Result

| | A0 `93a6433f` | B `6291292e` | change |
|---|---:|---:|---:|
| **GPU frame** | 16.490 ± 0.497 ms | **11.434 ± 0.420 ms** | **−5.056 ms, −30.7%** |
| **CPU frame** | 17.411 ± 0.471 ms | **12.705 ± 0.188 ms** | **−4.706 ms, −27.0%** |
| **avg fps** | 57.5 ± 1.5 | **78.7 ± 1.2** | **+21.2, +37.0%** |
| Grass prepass | 1.771 ± 0.340 ms | 1.053 ± 0.100 ms | −40.5% |
| Grass colour | 2.694 ± 0.298 ms | 1.304 ± 0.156 ms | −51.6% |
| Grass shadow | 1.058 ± 0.127 ms | 0.640 ± 0.112 ms | −39.5% |
| **grass total** | 5.522 ms | **2.997 ms** | **−45.7%** |

**37% more frames per second in the same scene.** The CPU frame moved with the GPU frame,
which is what a GPU-bound scene does: the CPU spends its frame in `swap`.

Two things worth noting beyond the headline. The run-to-run spread **tightened** (GPU
±0.497 → ±0.420 ms, CPU ±0.471 → ±0.188), so the frame is steadier as well as shorter. And
`Grass shadow` fell 0.42 ms here even though the isolated A/B of the shadow-indexing change
measured **no** movement (0.895 → 0.892 ms) — so that gain came from somewhere else in the
57 commits, and this document does not claim to know where. Recorded rather than explained.

## DLSS is a quality setting here, not a performance one

Measured on the same build and scene, three runs, no environment override:

| | GPU frame | avg fps |
|---|---:|---:|
| `WGR_DLSS=0` (bilinear) | 11.434 ms | 78.7 |
| default (DLSS active, quality 2) | 12.271 ms | 73.0 |

DLSS genuinely initialises — `dlss_route true`, `dlss_active true`, NGX reports Super
Resolution available, and the process exits 139 at teardown, which is the known NGX crash
and the only reliable witness that DLSS actually ran.

**And it costs about 0.84 ms rather than saving anything**, because **the internal render
resolution is 1072×603 in both cases.** Turning the upscaler off does not raise the render
resolution; it only changes *how* the same reduced image is scaled up. In that arrangement
DLSS can only add its reconstruction pass on top, so it buys image quality and spends frame
time. This is consistent with the ~1.07 ms figure PERF-010 measured for it independently.

That is why both arms above are bilinear: it is the configuration in which the two builds
differ only by the code between them.

**It also answers a user report** — *"turning DLSS off in the dev tools makes no difference,
maybe fewer fps"*. Correct, and expected. DLSS as currently wired is not a framerate
feature. Making it one means letting the quality mode drive the render scale lower when
DLSS is on than when it is off, which is a design change and not a bug fix.

## What this does not say

- **One scene.** `perf_abel` is grass-heavy, which is where most of the day's work landed.
  A town or interior scene would show less. The frame-attribution table in the roadmap's
  Phase 3 section gives the per-scene spread.
- **Not attributed.** This is A0 against B across 57 commits. It does not apportion the
  5 ms between the sky SH fix, the grass indexing and everything else; the individual
  commits carry their own isolated measurements.
- **Bilinear, not native.** Neither arm renders at 1600×900 internally, so none of these
  numbers is a native-resolution figure.
