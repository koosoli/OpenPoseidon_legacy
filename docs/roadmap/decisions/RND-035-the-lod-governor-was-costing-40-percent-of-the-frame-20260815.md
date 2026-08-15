# RND-035 — The LOD governor was costing 40% of the frame, and the 35 s warmup hid it

**Date:** 2026-08-15
**Branch:** `feat/farfield-tier`
**Status:** MEASURED on Reforger Everon. Generalisation to Arma 3 / DayZ **not yet verified**.
**Related:** [RND-032](RND-032-single-lod-xob-export-cost-6-5x-of-gpu-frame-20260815.md),
[RND-033](RND-033-cross-layer-frame-profile-objects-are-not-the-bottleneck-20260815.md)

## The result

Settled Everon (`ev_DALL.wrp`), identical residency of 199,881 objects in both arms,
3 repeats each, cheapest steady frame quoted, one binary (levers are env vars):

| | `WGR_LOD_GOVERNOR_RANGE=16` (legacy) | `=4` (new default) |
|---|---|---|
| avg fps | **30.33** | **50.34** |
| GPU frame | 32.924 ms | 18.854 ms |
| main triangles | **56,037,316** | **9,056,831** |
| cascade 3 triangles | 20,241,420 | 16,605,599 |
| LOD histogram | `13800, 25899, 1, 0,0,0,0,0` | `1037, 3526, 4223, 15474, 13902, 1431, 77, 0` |

**+66% framerate**, at identical residency and identical instance counts.

**Attribution, stated precisely.** The legacy arm above moved FOUR levers at once
(governor, admission, texture eviction, far tier), so it does not by itself prove the
governor did it. The isolation arms settle it: changing the governor ALONE
(`D old-gov`, 30.51 fps / 32.075 ms) reproduces the legacy result almost exactly, while
`C no-far` (53.33 fps / 18.467 ms) and `E old-admit` (50.85 fps / 18.950 ms) are
indistinguishable from the default. The other three levers together account for
0.85 ms of the 14.1 ms difference. So the isolated governor effect is
**30.51 -> 50.34 fps**, and the four-lever comparison is merely consistent with it.

It also looks *better*: at range 16 the near trees render as sparse pale specks —
full branch geometry aliasing at multi-kilometre distance — where the coarse LOD is a
solid canopy.

## Why the governor was bistable

`Scene::AdjustComplexity` drives `_lodInvWidth` from a wall-clock framerate feedback loop
across a range clamped to `[quality, quality * RANGE]` (`Scene.cpp:585-586`). That value
multiplies distance in the LOD selector *and* in the GPU sub-pixel cull.

At range 16 the loop has enough authority to reach either extreme and stay there. Once the
frame is fast it pins every object to LOD 0/1 — 56M triangles — and the frame stays just
fast enough to keep it pinned. The histogram is the proof: `13800, 25899, 1, 0, …` is not a
LOD ladder, it is two rungs. At range 4 the same loop cannot overpower distance, and the
histogram spreads across all eight buckets.

## The methodology finding, which nearly shipped the wrong default

The benchmark recipe used a 35 s warmup. **At 35 s, residency on Everon is 21k of 199,914
and still climbing at ~700 obj/s; it does not reach `pending=0` until ~81 s.** So a 35 s
capture measures the streaming transient, not the world.

Measured at 35 s, the arms invert:

| | legacy | new default |
|---|---|---|
| GPU frame @35 s | **12.924 ms** | 22.879 ms |
| GPU frame @120 s | 32.924 ms | **18.854 ms** |

At 35 s the legacy arm looks like the *cheapest* configuration. Settled, it is the worst by
74%. **A 35 s capture would have shipped the wrong default with a straight face.**

Rule going forward: grep the residency log for `pending=0` before quoting any frame number
on a streaming world. "Cheapest steady frame" defends against GPU contention; it does not
defend against a world that has not finished loading.

## Where the CPU frame actually goes — and a retraction

RND-032 measured "GPU is 8.9% of wall clock at 200k objects" and this session repeatedly
treated that as the steady state, concluding that renderer work could not help and the CPU
object pipeline was the target. **That reading was wrong, and the correction is the more
useful finding.**

| phase | streaming (35 s, 21k resident) | settled (120 s, 199,881 resident) |
|---|---|---|
| CPU frame | 264.024 ms | 19.864 ms |
| `land:gnd` | 206.589 ms (78%) | **0.432 ms** |
| `swap` (CPU waiting on GPU) | 56.863 ms | 16.628 ms |
| every other phase combined | < 0.6 ms | < 4 ms |
| CPU − GPU | 241.145 ms | **1.010 ms** |
| GPU share | 8.7% | **94.9%** |

`land:gnd` drops **478x while resident objects grow 10x**. It scales with *objects created
per frame*, not objects resident, and not terrain — `drw:land` is 0.018 ms and `drw:obj` is
0.001 ms. At ~190 creations/frame it costs **~1.1 ms of CPU per object created**.

So the frame is CPU-bound *only while streaming*. On the settled world it is 95-100% GPU,
and GPU work is the correct target. The "9% of wall clock" figure describes a transient.

*(Note `land:gnd` is the terrain phase only by accident of call site:
`UpdateModernObjectResidency` runs inside `TerrainWgpu::DrawTerrain`. It is object
admission cost wearing terrain's label, which is part of why it stayed unattributed.)*

## The transient is now the headline problem

Everon: **1.2M placements, ~81 s to settle, frames over 200 ms throughout.** Chernarus has
2.97M placements; if the per-object cost holds, that is minutes.

Time-to-plateau, the only thing the admission lever moves:

| `WGR_OBJECT_STREAM_MS_PER_UPDATE` | cheapest | effect on settled frame |
|---|---|---|
| `4` (new default) | **79.4 s** | none (18.950 vs 18.854 ms, inside spread) |
| `0` (legacy 128/frame) | 97.5 s | — |

18.6% faster fill, and correctly no effect on the settled frame. It does not remove the
transient, because the cost is per object created and there are 200,000 of them.

## Far tier: works, is free, and does not fix pop-in

- 1,215,449 proxies from 1,216,138 placements; **0.099 ms cull + 0.017 ms draw**. The
  arm-to-arm frame delta (0.387 ms) is inside the run-to-run spread. Its real price is at
  load: 575-773 ms of build, 37.1 MB.
- Visually confirmed: it renders a continuous distant treeline along a coastline that is
  **completely bare** with the tier off. Near-field canopy is pixel-identical.
- **It does not reduce streaming fill-in pop.** Measured picture change between 35 s and
  120 s: tier-on 3.94%, tier-off 3.97% — identical. It cannot help, because fill-in happens
  *inside* the object draw distance, where the far tier is not what draws.

## Open

- Does the governor win generalise to Arma 3 Stratis and DayZ Chernarus? Unmeasured. If a
  world's legacy histogram is *not* pinned to LOD 0/1, the governor was never its problem.
- Chernarus time-to-plateau, and its `textureBytes` / `geometryCapacity` / `geometryRetired`
  split — the 4.23 GB claim in RND-033 has never been broken down, and the equivalent Everon
  figure turned out to be 84% geometry pool rather than textures.
- The ~1.1 ms per object created is unattributed within itself. Prime suspects are the
  synchronous ODOL parse and the per-mip texture re-read in `EnsureUploaded`
  (`TextureSourcePac::GetMipmapData` opens the stream once per mip level).
