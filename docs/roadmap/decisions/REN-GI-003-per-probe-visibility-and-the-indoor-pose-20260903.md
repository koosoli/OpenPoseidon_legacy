# REN-GI-003 — Per-probe visibility, and an indoor pose to judge it by

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-GI-001 (the probe volume), REN-GI-002 (the sun proxy)

## What it adds

Stages 1 and 2 let the hardware interpolate the eight probes around a shaded point. That is
what makes probe GI leak: a probe standing in the sun outside a wall is blended into a point
inside the room, because trilinear filtering knows nothing about the wall.

The probes now also record, per ambient-cube face, the **mean distance and mean squared
distance** of what their rays hit (a second 3D texture, the same six-slab shape). The shading
gathers the eight probes **by hand** instead of filtering, and weights each by

- its trilinear weight,
- a **facing** weight, so a probe behind the shaded surface contributes nothing,
- a **Chebyshev visibility** term from that probe's moments in the direction of the point: if
  the probe's rays that way stopped well short of the point, something stands between them.

If every surrounding probe is occluded the function returns zero weight and the analytic sky
ambient stands unchanged, which is the pre-GI look.

Defaults changed with it: probe spacing **4 m → 2.5 m** (an 80 m box instead of 128 m, because
GI matters near the camera and a barn is not 4 m wide) and the share of the old per-pixel
interior-sky darkening that still stacks on top **0.5 → 0.25** (the volume now does that job).
The volume-edge fade widened from one probe to two, so a fine grid cannot show a ring.

## The indoor pose

There was no interior scene in the bench set, so this decision added one: `stodola_open`, the
open barn on Everon at 5256 / 9375.5, found by listing the world's objects
(`PoseidonTools terrain objects`). Two missions ship with it: `gi_barn0/180.eden` (standing
inside) and `gi_out0/90/180/270.eden` (9 m outside, looking back at it). Azimuth 270 is the one
worth looking at: sunlit grass, the dark interior through the doorway, the roof above.

## Measured (Everon, noon, 3441×1440, VSync off)

| | GI off | GI on (2.5 m probes) |
|---|---:|---:|
| barn interior patch, mean luma | 17.6 | **18.1** |
| barn frame, mean luma | 59.3 | 55.6 |
| `perf_town` frame, mean luma | 87.9 | 85.5 |
| probe update | — | 0.14–0.16 ms |

At the previous 4 m spacing the interior went the *wrong* way (17.6 → 16.8): too coarse to
resolve a doorway, so the room only got darker. At 2.5 m with the interior share reduced, light
spills in through the opening and the interior comes up while the frame as a whole settles
about 4% darker. Outdoors the visible change is that tree canopies read denser; no seam or ring
at the volume edge.

## Two shader traps, both fatal at startup and both invisible to the old tests

1. **A `textureLoad` inside a helper function called from a loop** makes naga's SPIR-V backend
   panic with "Expression [n] is not cached" — at pipeline creation, i.e. `wgr_create` fails and
   the game has no renderer. The gather is written inline for that reason.
2. **`i32(dims.y)`, an integer cast of a component of a const-foldable vector**, makes wgpu's
   pipeline-constant pass declare the whole function invalid ("Function ... is invalid"), same
   consequence. The probe dimensions are separate scalar constants now.

naga's own validator accepts both. Only building a pipeline from the **real object shader**
reproduces them, which is what `gi_gather_survives_the_object_shader_backend` does — a toy
fragment shader over `frame.wgsl` compiles happily, because the gather is stripped as dead code.
That test now stands beside the compute and sun-proxy ones.

## Switches

`WGR_GI=0` off, `WGR_GI_WEIGHT`, `WGR_GI_SPACING`, `WGR_GI_INTERIOR_MIX`, `WGR_GI_RSM`,
`WGR_GI_RSM_GAIN`; all of them and more in the dev panel's GI tab.

## Not done

A room with a window rather than a barn door; the interior-sky share is still non-zero because
a 2.5 m grid cannot resolve a small room, and dropping it to 0 there would brighten interiors
the maps currently darken correctly.
