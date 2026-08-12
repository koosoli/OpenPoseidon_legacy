# WLD-019 — The LCA mask decoded: an indexed selector over six slots, and the three bugs behind it

**Date:** 2026-08-12
**Related:** WLD-017 (terrain materials ≠ ground layers), WLD-018 (satellite stage
dropped), MAT-040 + amendments (parallax is terrain-only; the mask is not a weight field)
**Status:** Measured and implemented. Composition renders correctly on Arma 2, OA and
Arma 3; still behind `WGR_TERRAIN_A3_SURFACES` (default 0) pending a look call.

## What the corpus says

MAT-040 amendment 2 established that the mask holds 0 or 255 and could not be a
weight field. This record finishes the decode.

Measured over **every shipped mask of both generations** — 64 Stratis masks
(67,108,864 texels) and 220 Takistan masks (57,671,680 texels), decoded twice
through independent readers (`PoseidonTools image convert` and Bohemia's own
`ImageToPAA`, which agree byte for byte):

```
Stratis (A3)                       Takistan (OA)
R  0 / 255 only                    R  0 / 255 only
G  0 / 255 only                    G  0 / 255 only
B  0 / 255 only                    B  0 / 255 only
A  255 (90.6%) / 128 (9.4%)        A  255 / 128 / 0

texels with >1 RGB channel high:   0.0000%   (both generations)
```

RGB is **strictly one-hot**. Alpha takes **three** authored levels. So the mask is
an indexed surface selector, and the index is:

| alpha level | RGB | selects |
|---|---|---|
| 255 | none | slot 0 (the base) |
| 255 | R | slot 1 |
| 255 | G | slot 2 |
| 255 | B | slot 3 |
| 128 | — | slot 4 |
| 0 | — | slot 5 |

Four surfaces in RGB plus two more addressed through alpha is exactly Bohemia's
own description of the format (<https://www.bohemia.net/en/blog/show-me-the-light>):
"by using all four channels of the ARGB mask and with some encoding tricks
employed, the total number of different surfaces on a single satellite segment
was increased from four to six".

**The alpha mapping is measured, not inferred.** Correlating each tile's mask
against the slots its own RVMATs name, across 284 tiles:

- alpha level 128 appears only on tiles that bind a 5th surface — **137/137**
  Takistan, **30/30** Stratis.
- alpha level 0 appears only on tiles that bind a 6th surface — **47/47** Takistan.
- **Zero counterexamples.**

The converse does not hold (a tile may declare a slot its mask never selects at
measurable frequency), which is expected: the slot set is the union over a tile's
RVMAT variants.

## Two shader families, not one

The families differ in how far the stage stack runs, and this is why slots must be
read by stage index:

| | Arma 2 / OA | Arma 3 |
|---|---|---|
| `PixelShaderID` | `TerrainX` (3,531/3,531 sampled) | `TerrainSNX` (478/478) |
| surface slots | **six**, Stage4..Stage14 | **five**, Stage4..Stage12 |
| Stage14 | a surface colour (696/696 sampled) | the whole-tile normal `n_<x>_<y>_no.paa` |

Confirmed at runtime: Stratis reports 478 tile normals and 0 sixth-slot use;
Takistan and Zargabad report 0 tile normals and 344 / 57 materials using the
sixth slot; Chernarus 555.

## Three bugs, each independently fatal

**1. Slots were assigned in encounter order.** `LoadOprw25` skipped empty stages
and packed named colours down into a dense list. A tile that leaves a stage empty
— `p_015-010_n_l09_l13_n_l10_l11` leaves slots 0 and 3 empty, and holes are the
norm: 33,107 of Takistan's 17,655 RVMATs' stage tokens are `n` — had every slot
after the hole renumbered, so the mask selected the wrong surface. Takistan
preserves **4,796 holes** once slots are read by stage position.

**2. Only five slots existed, so Arma 2's sixth was dropped**, on 447 of
Takistan's 484 tiles.

**3. The mask's alpha was read as a fifth weight.** Alpha sits at 255 over ~90% of
texels, so slot 4 took full coverage nearly everywhere and the base took none.
On Arma 2 it was worse: OA ships many masks as **DXT1, which has no alpha at
all**, so the decoder's constant 255 covered every tile completely. This is the
washed-out ground with banding along the mask's hard edges.

## And two more the fix exposed

**4. `uvTransform` was read as row vectors.** It is a basis: `aside`/`up`/`dir` are
where the source's X/Y/Z axes *go*, so u is the first component of each of the
three, not all three components of `aside`. The row reading is harmless for the
`tex` stages (diagonal matrices) and destroys every `worldPos` stage: Stratis's
satellite TexGen is `aside=(k,0,0) up=(0,0,k) dir=(0,-k,0)`, so v came from the
terrain **height**. The satellite smeared along the slope, and the mask — which
shares that TexGen — selected one surface for a whole tile. A compensating
`(x,z,y)` swizzle in the shader had been added on top of it; both are gone.

**5. The detail fade measured distance from the world origin.** `sample_authored_cell`
was handed the *absolute* world position and used `length()` of it as the camera
distance. On every real world that is thousands of metres, so `detail_fade` was 0
from the first metre and **the authored surface stack had never been drawn at
all** — the only visible effect the flag ever had was satellite-instead-of-legacy.
That is why the previous session measured turning it up as a 0.047/255 mean
change. Absolute position (for the TexGens) and camera-relative position (for
distance and the view vector) are now separate parameters.

## The decode

The three alpha levels are interpolated as a partition of unity, so a filtered tap
between two authored levels crossfades instead of decoding as a level nobody
painted. RGB one-hot values filter to exactly the local coverage fraction, which
is where the smooth blend comes from — the mask is binary per texel by design.
Weights for slots whose stage is a hole are not spent; the total is renormalised
over the slots that resolved.

## Measured result

| world | rev | materials bound | complete | ground layers | dropped |
|---|---|---:|---:|---:|---:|
| Takistan | OPRW24 | 3,292 / 3,293 | 3,292 | 998 | 0 |
| Zargabad | OPRW24 | 1,350 / 1,351 | 1,350 | 988 | 0 |
| Chernarus | OPRW24 | 1,024 / 1,025 | 1,024 | 2,076 | 0 |
| Stratis | OPRW25 | 478 / 479 | 478 | 706 | 0 |

Takistan was 2,999 bound / 499 layers before; the rise is the sixth slot and the
4,796 recovered holes binding their own images.

**The ground-layer cap had to move, and the number says why.** The budget is
dominated by per-tile images — every satellite segment contributes its own `_lco`
and `_lca` — so it scales with map *area*, not with surface variety. 1,024 cleared
both Operation Arrowhead worlds by a hair and then dropped **1,052** of
Chernarus's bindings, leaving 498 of its 1,024 materials incomplete: base Arma 2's
flagship terrain half untextured. At 4,096 Chernarus binds completely, and that
still sits under the 8,192 bindless-texture device limit the renderer already
requests unconditionally for the object array.

## Status of the composition itself

Visually it is now clearly ahead of the legacy path on both generations at every
range tested (captures in `docs/roadmap/evidence/`): Takistan's aerial resolves
real fields, tracks and rocky slopes instead of flat sand and green polygons;
Stratis resolves scrub, dirt tracks and beach instead of uniform sand; ground
level finally shows the authored gravel surfaces at all.

It remains **off by default** (`WGR_TERRAIN_A3_SURFACES=0`) because promoting it
changes the default look of two whole generations, which is the owner's call. The
0.35 cap is gone — that was a guard rail around a mask read as weights — so the
override now spans the full 0..1.

Left standing, and deliberately not tuned here: close-range surface tiling is
visible on Takistan (the `tex`-source TexGen scale is ×10 per land cell and has
not been checked against Real Virtuality's own terrain UV), and the authored
ground reads brighter than the legacy path. Both are look questions that only
mean something once the composition is on.

## Fallback

`surface_count == 0` early-returns to the legacy cell path byte for byte, which is
every OFP world — OFP terrain has no authored materials at all. The OFP preview-0
WGPU gate and the original-training mission capture both pass unchanged.
