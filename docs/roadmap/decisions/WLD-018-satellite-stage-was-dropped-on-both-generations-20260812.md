# WLD-018 — The terrain satellite stage was dropped on both generations

**Date:** 2026-08-12
**Related:** WLD-017 (ground-layer index spaces), MAT-034
**Status:** IR capture landed. Renderer consumption still gated off; two negative
results recorded below.

## What the corpora actually say

Reading the raw RVMATs rather than inferring from behaviour:

```
Takistan   ca\takistan\data\layers\p_011-010_*.rvmat      PixelShaderID TerrainX
  Stage0  ca\takistan\data\layers\s_011_010_lco.paa       satellite,  TexGen worldPos
  Stage1  ca\takistan\data\layers\m_011_010_lca.paa       mask,       TexGen worldPos
  Stage2  #(rgb,1,1,1)color(0.5,0.5,0.5,1,cdt)            procedural
  Stage3/4    tk_hlina_nopx  / tk_hlina_co                surface 1: normal + colour
  Stage5/6    tk_skala_nopx  / tk_skala_co                surface 2
  Stage7/8    tk_trava_nopx  / tk_trava_co                surface 3
  Stage9/10   tk_valouny_nopx / tk_valouny_co             surface 4
  Stage11/12  tk_mountain_nopx / tk_mountain_co           surface 5

Stratis    a3\map_stratis\data\layers\p_003-000_*.rvmat   PixelShaderID TerrainSNX
  Stage0  a3\map_stratis\data\layers\s_003_000_lco.paa    satellite
  Stage1  a3\map_stratis\data\layers\m_003_000_lca.paa    mask
  Stage2  #(rgb,1,1,1)color(0.5,0.5,0.5,1,cdt)            procedural
  Stage3/4    gdt_strbeach_nopx  / gdt_strbeach_co        surface 1
  Stage5/6    gdt_strseabed_nopx / gdt_strseabed_co       surface 2
  Stage14 a3\map_stratis\data\layers\n_003_000_no.paa     per-tile terrain normal
```

The two generations are the **same structure under two shader names**. This is a
shared Real Virtuality convention, not an A2/OA one, and the handover's warning
about forcing OA through A3 assumptions does not apply to these stage roles —
they are measurably identical.

Two differences are real:

- Arma 3 adds a **per-tile terrain normal** at Stage14 (`n_<x>_<y>_no.paa`).
  Takistan has none; its normals are the per-surface `_nopx` maps at the odd
  stages.
- Takistan uses five surfaces where this Stratis tile uses two, which the mask's
  four channels plus the unpainted base already accommodate.

## The defect

`LandSave` matched Stage0 with `find("_co.paa")`. The satellite is named
`_lco.paa`, which does not contain that substring, so **Stage0 never matched on
either generation** and `A3TerrainMaterial::satellite` was always empty. The
renderer then fell back to the legacy cell texture for the satellite slot, which
is why an earlier trace showed `sat=<empty>` on Stratis and nobody noticed: the
fallback is invisible when the authored path is disabled.

Accepting `_lco.paa` captures it. Ground layers rise from 499 to 982 on Takistan
and from 76 to 140 on Stratis — the satellite tiles, previously never uploaded.
Neither render changes, because the authored composition is still gated behind
`WGR_TERRAIN_A3_SURFACES` (default 0).

## Negative result: the satellite is not a cell texture

Binding the captured satellite as the *legacy cell texture* was tried and is
wrong. The satellite is one low-frequency image per 1,024 m tile carrying its own
worldPos TexGen; the legacy slot is sampled through the 50 m land cell's frame.
Doing it tiled a whole tile's image into every cell and produced a corduroy
pattern over the entire map. The satellite belongs to the authored path, which
carries the TexGen that gives it meaning. Reverted, and the reason is recorded in
the code so it is not retried.

## Headroom is now the constraint

982 of 1,024 ground layers on Takistan. That fits only because WLD-017
deduplicated the array; before it, this would have been hopeless. A larger OA
world (Zargabad, or Chernarus if it is ever supplied) has more 1,024 m tiles and
will exceed the cap. The binder drops and counts rather than corrupting, so the
failure is visible in the existing log line — but raising
`WGR_TERRAIN_MAX_GROUND_LAYERS`, or not binding the satellite while the authored
path is off, is the next decision this path needs.

## Regression evidence

- Takistan day capture: mean 0.12/255 against the previous commit — unchanged, as
  expected for an IR-only change.
- Stratis airbase: mean 0.47/255, animated water and cloud only. 419 of 419
  authored blends still complete.
- OFP Preview-0 WGPU smoke passed.

## Still open

Terrain normals reach neither IR nor renderer on either generation: A3's per-tile
Stage14 and A2's per-surface odd stages are both dropped. That is the next piece
of the normals backlog, and like the stage roles above it is shared work rather
than an OA-specific port.
