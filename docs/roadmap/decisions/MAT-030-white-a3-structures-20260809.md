# Why Arma 3 structures draw white

**Date:** 2026-08-09
**Ticket:** MAT-030 / MAT-020
**Status:** Isolated to one binding that never fires. Not yet fixed.

## Symptom

The Arma 3 castle ruin and the `bricks_v1_f` prop both render as untextured white
geometry in the production WGPU path, while `stonesharp_big` (a rock) renders correctly
in the same frame.

## What separates the ones that work

The rock's *face* texture is a real albedo, `a3\rocks_f\data\stonesharp_big_co.paa`. It
never needed its RVMAT to look right.

The two white models carry **placeholder face textures** — `wip_mlod_co.paa` and
`castle_01_ruins_mlod_co.paa` — with the real albedo living in the material instead. So
they depend entirely on the RVMAT's base colour reaching the draw.

## The binding that should supply it does not run

`EngineWgpu::DrawSectionTL` has a production base-colour binding: when a section's
translated material has a non-procedural `BaseColour` slot with a loadable path, it loads
that texture and overrides `d.texture_id`, logging `Stage0 BaseColour resolved` once per
material.

That log appears **0 times** across both smoke runs, including the run where the harness
itself reports the brick material resolving cleanly:

```
slot BaseColour stage 0 uv=tex texture=a3\...\wip_bricks_co.paa exists=true procedural=false
```

So the slot is present, non-procedural, has a path, and the file exists — and the binding
still produces nothing. The draw keeps the placeholder face texture, which is white.

## Narrowing it

The gate is `materialBindingTarget && section.surfMat`, and that branch is known to run in
daylight — a trace placed inside it fired for the pine in the same build. The remaining
candidates inside the branch are:

1. `translated.Find(BaseColour)` returning null at draw time even though the harness's
   parse of the same file finds it — which would mean the cached translation differs from
   the harness's;
2. `_wbank->Load(...)` returning a texture whose `EnsureUploaded()` yields 0, so
   `baseHandle` is false and the bind is skipped silently.

(2) is the more likely of the two and is trivially testable: log the `EnsureUploaded()`
result rather than only the success path. The current code logs *only* when the handle is
non-zero, which is precisely why a failure here is invisible.

Note this is not the `Multi` family gap. The castle's drawn section uses
`castle_mlod.rvmat`, which declares `stages=0` — a placeholder material — so `Multi`
support would not fix that model. Two different problems wear the same white.

## Next step

Instrument the failure branch of the Stage0 bind (handle == 0) and re-run the brick smoke.
One capture distinguishes "slot missing at draw time" from "texture failed to upload".
