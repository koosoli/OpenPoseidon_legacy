# GRS-011 — GTAO reads a dense canopy as a cave, and the fix is a weight, not an exemption

- **Ticket:** GRS-011 (foliage lighting, WGPU)
- **Date:** 2026-08-25
- **Status:** fixed and deployed. A/B captures retained in `.tmp-a1-tank/`
  (`tree-wgpu.png` before, `tree-gl33.png` reference, `tree-noao.png` ablation,
  `tree-fixed.png` after).

## The report

Owner, playing the T-72 comparison mission on Abel: "trees canopy is a bit
dark", with a screenshot of an oak whose entire interior renders near-black
while its sunlit crown is correct. Distant trees look fine; the darkness is a
close/mid-range effect.

## The A/B that named the mechanism

Same fixture (`Dta\Data3D.pbo` → `str dub.p3d`), same pose, `--test-model`
distance 18 on `devtest.abel`:

| arm | canopy interior |
|---|---|
| WGPU, stock | **near-black void** filling the tree's core |
| GL33 (`--render gl33`) | uniform mid-dark green — the reference |
| WGPU, `WGR_GTAO=0` | matches GL33's character |

One ablation flag restoring the reference makes GTAO the whole mechanism —
not the sky bake, not the interior-sky term, not the two-sided flip.

## Why GTAO does this to a tree

The horizon march walks the depth buffer looking for occluders. A canopy is
hundreds of alpha-tested leaf cards facing every direction: from any point
inside it, the march finds geometry at every angle and the ambient term
collapses toward zero — the same answer it would give inside a cave. But a
leaf card is not a wall. Most of what the march counts as occlusion around a
leaf is *other leaves*, which transmit and scatter light — the foliage
translucency path exists in the same shader for exactly that reason.

## The fix (`shading.wgsl`, `amb_ao`)

GTAO is applied to foliage at reduced weight instead of full strength:

```wgsl
var gtao_t = gtao_ao(frag_coord);
if (is_foliage) {
    gtao_t = mix(1.0, gtao_t, 0.35);
}
```

- **Weight, not exemption.** `WGR_GTAO=0`-style exemption would also delete the
  real contact darkening where canopy meets trunk and ground. At 0.35 the
  darkest interior sits at ~0.65 ambient, which is the GL33 capture's character
  at the same pose, while contact shading survives.
- **Same gate as the precedents.** `is_foliage` is the MapType-gated
  vegetation-cutout flag this shader already uses for the two-sided normal
  flip and the cavity exemption — fences, decals and characters keep full AO.
- Nothing else in the AO product changes: sky-vis, interior-sky and the baked
  term still multiply in unmodified.

## Scope and what was not touched

- WGPU only; GL33 has no GTAO and was already correct.
- The 0.35 constant is in-shader, not a uniform. Making it a `frame.foliage*`
  knob is cheap if tuning is ever wanted; a knob nobody asked for was not
  worth an ABI field.
- Grass receives the same reduction (it classifies `is_foliage`); no adverse
  effect visible in the capture — grass is mostly lit by the terrain path
  anyway.

## Related but separate (measured the same session, not fixed here)

The owner's tank screenshot shows **grass blades drawing over vehicle hulls**
and sky patches through a turret — that is the alpha-routing /
grass-over-object defect family (see the standing task on
`wgpu-weapon-parts-routed-to-blend`), not this mechanism. The canopy fix
neither helps nor harms it.
