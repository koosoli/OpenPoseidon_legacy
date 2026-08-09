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

---

# Answered: the section reaching the draw has no material (same day)

Both candidate explanations above are wrong, and the instrumentation that settled it is
now permanent — the bind's failure branches log instead of failing silently.

Running the brick prop alone at 8 m, the only material that reaches the base-colour
binding at all is `#RifleMetal`, the player's weapon:

```
Wgpu material: no bindable Stage0 for #RifleMetal (present=0 procedural=0 emptyPath=0)
```

The brick material never appears — not as a success, not as an upload failure, not as a
missing slot. So `EnsureUploaded()` is not returning 0 and `Find(BaseColour)` is not
returning null. The enclosing condition `materialBindingTarget && section.surfMat` is
simply false, because **the section that reaches the draw carries no `surfMat`**.

That is the same shape of result as the night trees, where no Arma 3 section reached
`DrawSectionTL` either. Two symptoms, one underlying fact: the material the *harness*
reads off the shape at load time is not the material the *draw* sees.

The harness enumerates `shape->GetSection(i).surfMat` per LOD directly from the loaded
shape, and reports `section 0 material a3\...\wip_mlod.rvmat exists=true`. The draw
receives sections via the vertex buffer's section list. If those two disagree — different
LOD, or a section list rebuilt without carrying `surfMat` across — everything observed
here follows, including why the pine works (its drawn sections evidently do carry theirs)
and the bricks do not.

## Next step

Log, once per shape, the drawn LOD's index alongside `section.surfMat` and the face
texture name, and compare against what the harness enumerates for the same model. That
identifies whether the draw is on a different LOD or on a section list that lost its
material, and those need different fixes.

---

# Root cause: the drawn shape has no texture, no material and no vertex buffer

Traced through `Shape::Draw`'s non-T&L fallback and then reverted. Running the brick prop
alone at 8 m, it reaches that fallback as:

```
TRACEFALLBACK non-TL path tex=<NOTEX> surfMat=<NULL> tl=1 hwtl=1 tlAble=1 buffer=0
```

Every part of that line matters:

- `buffer=0` — the shape has **no vertex buffer**. That is the only failing term in
  `engine->GetTL() && EnableHWTLState && tlAble && _buffer`, which is why this model takes
  the software fallback at all while the pine beside it takes the hardware path.
- `tex=<NOTEX>` — the drawn section has **no texture**.
- `surfMat=<NULL>` — and **no material**.

So the model does not draw white because a material failed to bind. It draws white because
there is nothing bound at all: no buffer, no texture, no RVMAT. Confirmed against the same
capture, where the Arma 3 pine renders correctly and its sections reach `DrawSectionTL`
with both their textures and their RVMATs intact.

This also disposes of the `Multi` theory, the placeholder-face-texture theory, and the
Stage0-bind theory in one go. None of them were the cause; all three were downstream
observations of a shape that arrived empty.

The white shapes being **boxy** is consistent: what is drawn has the silhouette of a
coarse hull rather than the authored prop.

## What this means for the other symptoms

The night brightness has the same signature — no Arma 3 section reached `DrawSectionTL`,
and the tree was drawn anyway, bright and untextured. The probe here shows exactly how
that happens: a shape with no buffer falls to a path that binds no material, so the draw
keeps the default white `_curMaterial`. That is very likely one bug, not two.

## Next step

Find why `_buffer` is null for these shapes while it is populated for the pine and the
rock. Candidates, in order: the vertex buffer is never created for this shape (upload
skipped or failed), or it is created on a different Shape instance than the one being
drawn — the harness enumerates textures and RVMATs successfully off *its* instance, so the
two are demonstrably not the same view of the model.

---

# The empty shape is an empty LOD: `bricks_v1_f` loads LOD0 with no sections

The previous section asked why `_buffer` is null and the drawn section has neither texture
nor material. The answer is upstream of the renderer entirely.

`bricks_v1_f.p3d`, ODOL 73, as read by this codebase's own loader:

| LOD | resolution | points | faces | textures | sections/materials |
|---|---|---:|---:|---:|---|
| 0 | 1 | 766 | 458 | **0** | **none** |
| 1 | 2 | 419 | 223 | **0** | **none** |
| 2 | 3 | 24 | 12 | 1 | `wip_mlod.rvmat` (Super, 8 stages, all slots resolving) |
| 3 | 10000 | 192 | 256 | 1 | — |

LOD 0 has 458 faces and **no sections at all**. That is not a plausible authoring state:
every face belongs to a section, and LOD 2 of the same model carries a complete Super
material whose every stage resolves. The high-detail LODs are being loaded with their
geometry intact and their section/texture tables empty.

Everything downstream follows mechanically. `Shape::Draw`'s hardware path requires
`NSections() > 0`; with none, the model falls to the non-T&L path, which is where the
probe caught it as `tex=<NOTEX> surfMat=<NULL> buffer=0`. Nothing is bound because there
is nothing to bind, and an untextured hull is white.

The standalone `PoseidonTools model inspect` reports the same `Textures: 0` for LOD 0,
which is expected — it shares this loader. So this measurement identifies *where* the data
is lost, not yet whether the file or the reader is at fault.

## Not a general ODOL 73 failure

The Arma 3 pine is also ODOL 73 and its LOD 0 loads two textures and two sections, both
with working RVMATs. So the loader reads ODOL 73 section tables correctly in general and
fails on this file specifically — the same shape of defect as
`research_house_v1_f.p3d`, which this loader refuses outright while `stonesharp_big.p3d`
and `castle_01_house_ruin_f.p3d` load.

## Next step

Distinguish file from reader using the independent implementation already checked out
locally (`.tmp-uksfta-bis/BIS.P3D`). If that reader finds sections on LOD 0, this is a
reader bug with a specific trigger; if it finds none either, the model's high LODs really
are section-less and the white is authored content the harness should skip.
