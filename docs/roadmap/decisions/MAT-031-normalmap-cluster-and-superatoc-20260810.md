# MAT-031 — the `NormalMap*` cluster, `SuperAToC`, and what "white on Stratis" actually is

- **Ticket:** MAT-031
- **Date:** 2026-08-10
- **Follows:** MAT-030 (family census), the specular-constant scaling in `4b4bd5f`

## Why this order

MAT-030 recommended the `NormalMap*` cluster next by corpus population. Running the
world instead of the corpus said the same thing for a different reason, and also
said that most of what looks like a coverage gap is not one.

Stratis was captured at Air Station Mike-26 with the per-material albedo diagnostic
on. It reports, once per material, why no albedo was bound. First run:

| reason | materials |
|---|---:|
| no BaseColour slot | 294 |
| uploaded 0 | 84 |
| procedural | 7 |

## The 84 "uploaded 0" were not a texture bug

Every one of the 84 named a texture under `a3\structures_f\data\...`, and every one
of those lives in `structures_f_data.pbo` — which the `@a3stratis` mod set did not
contain. 79 of them were `Multi` materials, so the family that looked like the
largest remaining texture failure was in fact an unmounted archive.

Hard-linking `structures_f_data.pbo` into the set removed the entire bucket:
`Multi` failures went 80 → 1, `Super` 161 → 163 (the two are the same materials
re-classified from "uploaded 0" to "no BaseColour slot"), and total failures
385 → 304 with **zero** remaining upload failures.

This is a harness fact, not an engine change, and it is the kind that reads as a
renderer defect for as long as nobody checks. A texture whose PAA decodes fine and
whose archive is absent fails in the same place as one whose format is unsupported.

## "No BaseColour slot" is almost always the correct answer

294 of 304 is the largest bucket, and for the families in it, it is what the format
says should happen. Measured in the corpus: `Super` declares a Stage0 in 43 of
2,853, `TreeAdv` in none of 369, `NormalMapSpecularDIMap` in 13 of 111. For those
materials the **face texture is the colour** and binding nothing from the RVMAT is
right.

So the diagnostic now reports the fallback alongside the reason. The first cut
classified it by NAME — `*_mlod_co` reads like an author placeholder — and that was
wrong, in the way this repository keeps warning about: it called 41 materials
suspect on the strength of a filename. The `_mlod_co` textures Arma 3 ships are
megabyte-sized baked art that uploads and renders fine. The verdict is now whether
the fallback texture actually **uploads**, and the name is reported beside it as a
note that decides nothing.

Measured that way, over the whole Stratis object set (the census is world-wide, not
view-dependent — three camera positions produced byte-identical counts):

| fallback | materials |
|---|---:|
| `face-ok` | 296 |
| `none` | 3 |
| `face-failed` | 2 |

**Five materials on Stratis cannot resolve a colour at all**, and each is a
different thing:

- `a3\data_f\penetration\{metal_plate,concrete}.rvmat` — penetration materials,
  which have no texture by design. That they reach a visual draw at all is a
  LOD-selection question, not a material one.
- `a3\data_f\lights\building_{landinglights_white,warninglight_red}.rvmat` — the
  *face* texture is a procedural constant, `#(argb,8,8,3)color(1.0,0.95,0.85,1.0,co)`.
  Nothing in this build synthesises procedural textures, so a surface whose whole
  appearance is a flat authored colour has nothing to draw.
- `a3\structures_f\walls\data\mil_wallbig_multi.rvmat` — a `Multi` whose Stage0 is
  procedural and which has no face texture either.

Three of the five are the same missing capability: `#(...)color(r,g,b,a,TAG)` is a
generated constant texture, and generating it is bounded work. That is the next
coverage item, and it is a much smaller one than "some structures are still white"
suggested.

## What did need a schema

Two things the census names, both measured over the whole 45,304-material corpus
(re-derived here; the family populations reproduce MAT-030's exactly):

**`SuperAToC` (n=8, 7 spelled `SuperAToC` + 1 `SuperAtoc`)** is Super stage for
stage — Stage1 NOHQ, Stage2 DT, Stage3 MC, Stage4 AS, Stage5 SMDI, Stage6 fresnel,
Stage7 `env_land_co` — with one difference that matters: **Stage0 colour is present
in 7 of 7**, where Super's is present in 43 of 2,853. These are the alpha-cutout
surfaces (wire fences, barbed wire, metal doors) that keep both colour and cutout
alpha in the material. Four of them are on Stratis and bound nothing.

**The `NormalMap*` cluster** names its own stage list: everything after "NormalMap"
is the stages following Stage1, in order. Six families corroborate each other:

| family | n | Stage0 | Stage1 | Stage2 | Stage3 | Stage4 |
|---|---:|---|---|---|---|---|
| `NormalMapSpecularDIMap` | 111 | `_co` 13/111 | normal 111 | SMDI 108 | — | — |
| `NormalMapDetailSpecularMap` | 42 | absent | normal 42 | DT 42 | SM/SMDI 42 | — |
| `NormalMapSpecularMap` | 32 | absent | normal 32 | SM/SMDI 32 | — | — |
| `NormalMapDiffuse` | 27 | absent | `_nopx` 26 | **`_co` 26** | — | — |
| `NormalMapDetailSpecularDIMap` | 12 | absent | normal 12 | DT 12 | SMDI 12 | — |
| `NormalMapMacroASSpecularDIMap` | 4 | `_ca` 1/4 | normal 4 | MC 4 | AS 4 | SMDI 4 |

`NormalMapDiffuse` is the trap and the reason each family gets its own table: it is
the only one carrying an albedo, and it puts it at **Stage 2**, where its siblings
put specular. Reading the cluster as one shape would bind a ground colour into
`SpecularDetail` — plausible, silent, wrong.

Stage0 is mapped only for `NormalMapSpecularDIMap` (13 witnesses). For the families
where not one material declares one, a `BaseColour` there would be invented rather
than measured. `NormalMapDetailMacroASSpecularDIMap` (n=2) is left unmapped
entirely, the same bar that left Super's and Skin's Stage7 tails alone.

## `_nopx` is `_nohq` with a payload in R

`NormalMapDiffuse`'s Stage1 is `_nopx`, a suffix the schema did not recognise.
Measured by decoding all 31 `_nopx` textures in `map_data`: G and A span the full
range and satisfy `x^2+y^2 <= 1` in **31 of 31**; B is near-constant high (mean
226–254). That is the DXT5nm signature `MaterialChannels` already records for NOHQ,
so `_nopx` earns the ordinary `NormalMap` slot and its measured decode.

It differs in exactly one channel: R, which NOHQ holds flat at 0 and these vary over
a narrow band — the "PX" payload. Nothing here samples it. Recorded rather than
assumed away, because the `NormalMap` slot declares R unused and that is now true
only in the sense that nothing reads it.

## Consequence for the temporary specular scaling

`MaterialSpecularScale` returns 1.0 for a family with no schema, so the 21
`NormalMap*Specular*` materials on Stratis — crash barriers, runway lights, road
decals — were keeping a **full** specular constant with no map to scale it, which is
the exact blowout `4b4bd5f` scaled away everywhere else. Registering the schemas
brings them under the same 0.25 approximation.

That is a side effect, not the fix. These families' names say they have specular
maps, and now the schema binds them into `SpecularDetail` where a renderer can
reach them. Retiring `WGR_MATERIAL_SPEC_UNMAPPED` still needs the maps bound in
both draw paths — and the GPU-driven path, which owns essentially every Stratis
section, currently carries no per-section map beyond albedo at all.

## Evidence

Run logs under `docs/roadmap/evidence/` (`*.log` is gitignored, so these are local):

- `a3-stratis-albedo-census-20260810.log` — before
- `a3-stratis-albedo-census-b-20260810.log` — with `structures_f_data.pbo` mounted
- `a3-stratis-fallback-verdict-20260810.log` — with the schemas and the
  upload-based fallback verdict

Captures are deliberately **not** committed. PROVENANCE.md records that the 29
tracked `a3-*` PNGs depict Bohemia artwork and are a problem for the claim that no
Arma 3 assets are distributed; adding more would make it worse.

## Reproducing

The world is the harness. `--test-world <wrp> --test-world-pos X Z` against the
training mission with `-mod=@a3stratis`, then histogram the
`Wgpu material: no albedo` lines by family, reason and fallback. The mod set must
include `structures_f_data.pbo`; without it the run reports 84 texture failures
that are not texture failures.
