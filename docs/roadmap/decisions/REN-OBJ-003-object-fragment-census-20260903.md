# REN-OBJ-003 — The object fragment census: the cutout pass shades two fragments per covered pixel

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-OBJ-001 (the pass is shading), REN-OBJ-002 (a lite tier bought nothing)

## The instrument

The mip-feedback buffer's four header words were spare. Behind a pipeline constant
(`WGR_OBJECT_COUNT_FRAGMENTS=1`; the production shader carries no atomic) the object fragment
shaders count into them: 0 = every rasterised colour-pass fragment, 1 = cutout fragments that
**survived the alpha test** (the ones `shade()` runs for), 2 = depth-prepass fragments, 3 = the
prepass cutout variant. Both pipeline families count (per-draw and GPU-driven — the constant has
to be in both lists, see REN-OBJ-001's correction). The words accumulate for the life of the
buffer; the harvest divides the delta between two readbacks by the frames between them, so the
capture JSON's `objects.fragments` is per frame. A counting run's timings are not quotable.

## Everon `perf_combat`, 3441×1440 (4.96 M pixels), one counting run

| | per frame | per screen pixel |
|---|---:|---:|
| colour-pass fragments rasterised | 7.87 M | 1.59 |
| cutout fragments that survived the alpha test (shaded) | **4.31 M** | **0.87** |
| prepass fragments | 9.08 M | 1.83 |
| prepass cutout fragments | 8.80 M | 1.78 |

The cutout cards cover something like 40–50% of this frame, so **0.87 shaded cutout fragments
per screen pixel is about two shaded fragments per covered pixel**: the colour pass shades a
leaf-card fragment and then another one behind it. `Objects: colour alpha-test` 1.75 ms / 4.31 M
= 0.41 ns per shaded fragment (2.5 G fragments/s) — a full `shade()` at that rate is the GPU
working hard, not idling on a bad pipeline. Grass colour, the neighbouring pass, is 1.71 ms.

## Why two fragments per pixel after a depth prepass

The cutout colour shader `discard`s below the alpha reference. A fragment shader that can
discard disables the hardware's early depth test: the shader runs first, the depth test after.
The prepass has already written the front-most depth per sample, and the colour pass tests
GreaterEqual against it with writes off — so the hidden card fragments are rejected, but only
*after* they have been shaded. Every cutout fragment behind another card is a full `shade()`
thrown away.

## The lever this names

Force the early depth test on the prepassed cutout colour entry: with depth writes off the
shader cannot corrupt depth, so testing before shading is safe, and the hidden half of the
fragments never reaches `shade()`. WGSL exposes this through naga's `early_depth_test` entry
attribute; whether the version in this tree carries it is the next thing to check, and the
entry must be a separate one used only by prepassed (write-off) pipelines — forcing early depth
on a pipeline that writes depth would write the depth of fragments the shader then discards.
The prize on this scene is up to half of 1.75 ms; the same mechanism applies to the per-draw
path's cutouts (Chernarus+'s CPU-drawn trees) and to the grass colour pass, which discards too.
