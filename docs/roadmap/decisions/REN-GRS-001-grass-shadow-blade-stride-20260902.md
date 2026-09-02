# REN-GRS-001 — The grass shadow pass draws every second blade, and a fragment census to say why

**Date:** 2026-09-02
**Branch:** `asset-obs-001`
**Follows:** ARCH-001's frame table (grass 23.4% of an 11.26 ms GPU frame); PARKED.md's rule
that grass reopens only "with a corpus that measures differently"
**Authored by a subagent of this session; verified and committed by the coordinator after the
agent was cut off by the session limit.** Captures: `.tmp-grass-attr/`.

## The different measurement

A millisecond cannot say whether a pass pays per blade or per pixel. The placement counters
give instances and vertices; what was missing was fragments. `WGR_GRASS_COUNT_FRAGMENTS=1`
builds the grass pipelines with census twins of every fragment entry (near/mid prepass,
near/mid colour, near shadow), each adding one striped atomic per fragment into a small
storage buffer read back with the placement stats. Timings from a counting run are not
quotable (the atomics cost time and may cost early-Z); the counts are.

The census line in the log gives the triangle side directly:

```
grass census: near 28812 clumps x 120 tris = 3457440 tris/pass, mid 56310 x 36 = 2027160;
prepass+colour 10969200 tris; shadow 2 cascade(s) x 28812 x 120 (stride 1) = 6914880; frame 17884080
```

**The shadow pass was 39% of all grass triangles in the frame** — two cascades, every near
blade, full detail — and it has no fragment shader: in the shadow map a clump is a coverage
stipple, not twelve resolvable blades.

## The lever

`WGR_GRASS_SHADOW_STRIDE` (default 2, 1 = the old behaviour exactly, max 12). The shadow draw
takes every `stride`-th blade of the clump the colour pass draws — with its exact seed, angle,
height and bend, so it is the same blade — and widens each survivor by the stride, so the
shadow *coverage* the ground receives is preserved while the triangle count falls by the
stride. It is renderer-owned (`card_look.w`), so no C++ changes.

## Measured (`perf_combat`, 1600×900, RTX 3070, render thread off, one run per arm)

| region | stride 1 | stride 2 |
|---|---:|---:|
| Grass shadow | 0.691 ms | **0.264 ms** |
| Shadow cascade 0 | 0.710 | **0.387** |
| Shadow cascade 1 | 0.855 | **0.437** |
| shadow grass triangles / frame | 6.92 M | **3.46 M** |
| GPU frame total | 14.83 | 11.73 |

The cascade rows halving is the signal. The frame-total row is *not* — every region in that
capture came in lower, sky LUTs and AO included, which is GPU clock state between two single
runs, not the stride. Quote the cascades, not the frame.

## The picture

`crop_lowleft_s1_over_s2.png` and `crop_lowmid_s1_over_s2.png` (stride 1 above the red line,
stride 2 below) show the same ground stipple density under the grass. The amplified whole-frame
diff (`diff_s1_s2_x4.png`) lights up everywhere — clouds, trees, every blade — because two
captures a minute apart differ in wind time, cloud evolution and placement jitter; it measures
run-to-run noise, not the stride, and the same-arm control diff the agent planned did not get
captured before the session limit. The crops are the visual evidence; the diff is not.

## What ships

Default 2, `WGR_GRASS_SHADOW_STRIDE=1` is the A/B on one binary. Census off unless asked.

## Not done

Three runs per arm, the same-arm control diff, stride 4, and the per-pixel census numbers for
the colour passes (the counters exist; the run was not made). Grass stays closed as a
*colour-pass* target; this was the shadow pass, and it was opened by the measurement PARKED.md
asked for.
