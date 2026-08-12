# MAT-040 — Parallax has authored height on terrain, on both generations, and nowhere else

**Date:** 2026-08-12
**Related:** MAT-038 (retained normal maps), MAT-039 (Multi composition), WLD-017,
WLD-018 (terrain normals bound)
**Status:** Measured. Implementation scoped, not started.

## The question

Is parallax occlusion mapping worth implementing, and does Arma 3 support it?

## What the corpus says

Parallax needs a height field. Real Virtuality's normal maps are DXT5nm — Y in
green, X in alpha — which leaves red free. The `_nopx` suffix is the one that
uses it: "NO" plus a "PX" payload. Measured R channel, 256x256 resample:

| texture | generation | role | R |
|---|---|---|---:|
| `wall_01_nohq` | OA | object (rock) | **0 → 0** |
| `wall_brick01_nohq` | OA | object (brick) | **0 → 0** |
| `stonewall_01_nohq` | OA | object | **0 → 0** |
| `tk_hlina_nopx` | OA | terrain surface | 106 → 187 |
| `gdt_beach_nopx` | **A3** | terrain surface | 94 → 163 |
| `gdt_concrete_nopx` | **A3** | terrain surface | 96 → 124 |

Every object normal map holds R flat at zero. Every terrain surface map varies it.

And the split is not incidental. Counting normal-map suffixes across
`structures_f`, `rocks_f`, `plants_f`, `data_f` and `map_data` (3,874 entries):

```
_nohq   263      _nopx   31      _no   1
```

All 31 `_nopx` are in `map_data.pbo`. Not one object addon ships a single one.

## Conclusion

**Arma 3 supports parallax on terrain and not on objects — identical to Arma 2.**
The answer to "implement it for A3" is therefore the same piece of work as
implementing it for OA: one terrain implementation serves both generations, and
the `gdt_*` / `tk_*` surfaces are the same slot in the same composition.

Parallax on objects would have to synthesise height from the normal map. That is
not an upgrade — it swims under camera motion and softens exactly the detail it
claims to add — and this record exists partly so it is not attempted.

## Why it cannot be done yet, and the order it forces

The terrain fragment shader has no per-surface normal *or* height sampling at all.
Its normal is a central difference of the heightmap (`sample_normal`), which is
geometry, not surface detail. The authored per-surface maps reach the GPU as of
WLD-018 but nothing samples them.

They can only be sampled inside `sample_authored_cell`, because a surface's height
and normal are meaningless without the mask that says which surface a texel is.
And that function is gated: `a3_surface_blend` defaults to 0, is clamped to 0.35,
and fades out entirely between 25 and 120 metres.

So the order is forced, and each step is shared between the generations:

1. sample the per-surface normals already bound, inside the authored composition;
2. resolve the authored composition's own status — the 0.35 cap and the 25-120 m
   fade are an experiment's guard rails, and WLD-017 changed what it composites,
   so it has never been judged since it started binding correctly;
3. parallax on top, reading `_nopx` red, once there is a correct surface to
   displace.

Doing 3 before 2 would displace a surface that is still mostly the satellite
image, which is not a test of anything.

## Also worth recording

`ShaderSchema`'s suffix table already classifies `_nopx` as a normal map and notes
the PX payload is "unsampled here". That comment is now backed by a cross-generation
measurement rather than a single-corpus observation.
