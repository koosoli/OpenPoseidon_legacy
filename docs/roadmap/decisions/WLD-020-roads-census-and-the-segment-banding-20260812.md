# WLD-020 — Roads: the census, and what the banding is not

**Date:** 2026-08-12
**Related:** the handover's priority 5 ("roads: census first, classify missing-asset
vs material vs placement before changing anything")
**Status:** Censused and reproduced. Cause NOT yet isolated; this records the
classification and closes off three wrong answers.

## The census

Takistan places 13,501 road links over 122 segment models, all parsing. The
material side is small and entirely conventional — all 8 RVMATs in `roads_e`:

| family | n | stages |
|---|---:|---|
| `NormalMapSpecularMap` | 6 | S1 `_nohq`, S2 `_sm`/`_smdi` |
| `NormalMapDetailSpecularMap` | 2 | S1 `_nohq`, S2 procedural `(argb,8,8,3)color(0.5,0.5,0.5,1,DT)`, S3 `_sm` |

**Not one of them declares a Stage0.** The albedo comes from the model's face
texture, and `ShaderSchema` already encodes exactly that — both families map
Stage0 to `MaterialSlot::Count` with the comment "not one material in any of them
declares one, so a BaseColour there would be a slot invented rather than
measured". So the material side needs nothing.

A segment model is trivial: `dirt7_w7_a0_802_r500.p3d` is ODOL 49, 3 LODs, and
LOD0 is **10 points, 8 faces, 1 texture**.

**Classification, as the handover asked for: not a missing asset, not a parse
failure, not a material-family gap.** Everything resolves. It is appearance.

## What it looks like

Captured on the Takistan village road (camera 2651.7 848.3 383.0, looking at
2663.4 855.8 378.6): the road renders as a row of alternating light and dark
trapezoids with hard edges at the segment joins — a piano-key pattern running to
the horizon, one key per placed segment. The road texture itself looks right; the
segments do not agree with each other.

## Three answers ruled out

**It is not the ODOL 49 UV dequantisation nit**, which the handover's backlog
lists and which this looked like a candidate for. That code is real and it is
wrong:

```cpp
set.uv[i][0] = 1.52587890625e-05 * (u + 32767) * du + set.minU;   // 1/65536
```

An `int16` spans [-32768, 32767], so `(raw + 32767)` spans [-1, 65534]: the low
end lands *below* `minU` and the high end short of `maxU`. The correct mapping is
`(raw + 32768) / 65535`, which hits both ends exactly. Worth fixing on its own —
but the error is ~1.5e-5 relative, four orders of magnitude too small to produce
visible per-segment banding. It is a nit, and it stays a nit.

**It is not a material-family gap** — see the census above.

**And it is NOT established to be albedo rather than lighting**, which is the
correction worth recording. The obvious next step was the new Materials-tab
"Base Color Only" view, which strips lighting entirely. Captured, and the banding
survived it — but so did the road's *lighting*: in the same frame the buildings
went flat and unlit while the road did not change at all. Road sections are
on-surface routed (`render::IsOnSurfaceRouting`, the polygon-offset class that
`IsGpuOwnedSection` excludes from the retained set), so they draw on the CPU
complement, and the retained path's debug views never reach them. The view proved
nothing about roads, and reading it as "so it must be albedo" would have been
a wrong conclusion drawn from a tool that was not connected.

## What to do next

1. Extend the debug views to on-surface sections, or read a road segment's UVs
   and face texture directly. Either separates albedo from lighting; until one of
   them is done, that split is genuinely unknown.
2. If albedo: the likely mechanism is per-segment UV placement into the road
   texture — adjacent placements alternate orientation (the `a0`/`a2`/`a4`
   variants are curvature/rotation variants), so an asymmetric or non-tiling
   sample would alternate with them.
3. If lighting: on-surface sections take the polygon-offset path, and a per-object
   normal or offset difference would alternate the same way.

The ODOL 49 UV fix is worth doing regardless, separately, and with its own before
and after — precisely because it cannot be what is being seen here, so bundling it
with a roads fix would muddle two independent changes.
