# REN-OBJ-004 — The cutout colour pass tests depth before it shades: 1.77 → 0.29 ms

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-OBJ-003 (two shaded cutout fragments per covered pixel after the prepass)

## The mechanism

A fragment shader that can `discard` disables the hardware's early depth test: the shader runs,
then depth is tested. The object colour pass is drawn after a depth prepass that already holds
the front-most depth per sample, so every hidden leaf-card fragment was shaded in full and then
rejected. WGSL's `@early_depth_test(force)` (naga's `EARLY_DEPTH_TEST` capability, wgpu's
`SHADER_EARLY_DEPTH_TEST` feature) moves the test in front of the shader. That is only legal on
a pipeline that does **not** write depth — a forced early test would also write the depth of
fragments the shader then discards — which is exactly the prepassed case: the per-draw prepassed
pipelines already ran write-off; the GPU-driven main-view colour pipeline now gets a write-off
twin (its prepass draws the full in-frustum set, so the depth it tests against is complete).

Two entries, `fs_gpu_early` and `fs_main_early`, are wrappers of the existing bodies with the
attribute. They are compiled only under the `EARLY_DEPTH` define, which the composer sets when
the device offered the feature — the first attempt put them in the modules unconditionally and
naga refused the whole shader at startup on a device that had not requested the capability. The
renderer requests the feature when the adapter has it; without it the entries do not exist and
the switch is inert.

## Measured (Everon `perf_combat`, 3441×1440, three alternating runs per arm, same session)

| | late test (old) | **early test** |
|---|---:|---:|
| Objects: colour alpha-test | 1.77 (1.72–1.83) | **0.29** (0.28–0.30) |
| Objects: colour solid | 0.37 | **0.08** |
| GPU frame | 22.4 | **19.1** |
| CPU frame | 24.4 | 24.2 |

−1.48 ms on the cutout pass (84%), −0.29 on the solid pass, −3.3 ms on the GPU frame. The CPU
frame does not move because this pose is CPU-bound at the bench window; the GPU headroom is
what a DLSS-off high-resolution frame or a busier scene will spend.

## The picture

Full-frame difference between the arms: 1.6% of pixels differ by more than 32 levels, 0.03% by
more than 96 — grass and cloud motion between two runs. The crops (`.tmp-veg/ez_off_on_trees.png`,
`ez_off_on_ground.png`) are identical: pines, trunks, bushes, the soldiers on the slope.

## Decision

**ON by default** (`WGR_OBJECT_EARLY_Z=0` restores the late test). The Chernarus+ ground pose
(CPU-drawn trees on the per-draw prepassed pipelines) ran as the smoke check for the second
family.

## Not measured

One scene for the numbers; other worlds ran only as smoke. The same mechanism applies to the
grass colour pass, whose shader discards too and which shades a similar screen area for
1.7 ms — that is the next row on this line (REN-GRS-002).
