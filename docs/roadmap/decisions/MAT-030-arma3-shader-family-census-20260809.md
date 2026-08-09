# MAT-030 — what Arma 3 actually asks for: a full shader-family census

**Date:** 2026-08-09
**Ticket:** MAT-030
**Status:** Measurement only. No mapping is decided here; this exists so the next
family is chosen by population rather than by whichever one was noticed first.

## Method

Every `.rvmat` in every PBO of the local retail Arma 3 install (all DLC included) was
extracted and parsed: **45,304 materials, 0 parse failures**. Extraction is
`PoseidonTools pbo extract -f ".rvmat"`; parsing is a standalone raP reader, because
`PoseidonTools config debin` refuses RVMATs even though the engine loads them.

Nothing here is committed but the numbers — the corpus lives outside the repository.

## Families by population

| family | n | schema? |
|---|---:|---|
| `TerrainSNX` | 20,403 | no — terrain surface path |
| `TerrainX` | 17,071 | no — terrain surface path |
| `Super` | 4,598 | **yes** |
| `Multi` | 1,628 | no |
| `TreeAdv` | 381 | **yes** |
| `Normal` | 367 | **yes** |
| `Skin` | 347 | no |
| `TreeAdvTrunk` | 174 | **yes** |
| `NormalMapSpecularDIMap` | 111 | no |
| `NormalMapDetailSpecularMap` | 42 | no |
| `Grass` | 38 | no |
| (blank) | 37 | n/a |
| `NormalMapSpecularMap` | 32 | no |
| `NormalMapDiffuse` | 27 | no |
| 17 further families | ≤12 each | no |

The two terrain families are 83% of the corpus and belong to the terrain surface path,
not to object material translation — they should not be read as the biggest gap in this
ticket. Among object materials, `Multi` is the gap: 1,628 materials, more than four
times the whole TreeAdv group.

## Stage shapes of the unmapped leaders

Measured the same way as Super and TreeAdv — the tag on a procedural stage and the
suffix on a file stage, as two witnesses.

**`Multi` (n=1,628)** does not resemble Super at all. Stages 0–3 are four colour maps
(`_co` in 1,561/1,592/1,494/1,264), Stage4 is a mask (`_mask` 1,576), and Stages 5–8
are `_dtsmdi` — a combined detail+SMDI texture that has no slot in the current
`MaterialSlot` set. Every stage reports an empty `uvSource`. This is a layered/masked
material: four layers selected by a mask, each with its own detail-specular. It needs
a genuine design decision about layered materials, not another row in a table.

**`Skin` (n=347)** is Super-shaped but shifted, and must not be assumed to *be* Super:
Stage1 `_nohq` 347/347, Stage2 macro (`_mc` 228 + procedural mc 118), Stage3 `_co`
340/347 — a base colour at Stage3, where Super puts macro — Stage4 `_as` 341, Stage5
`_smdi` 347/347, Stage6 procedural in 347/347. The Stage3 colour is exactly the kind of
slot that a Super-shaped guess would get wrong.

**`NormalMapSpecularDIMap` (n=111)** is small and simple: Stage0 `_co` in 13, Stage1
normal 111/111, Stage2 SMDI 111/111.

## A note on Super's Stage3

506 of Super's 4,598 Stage3 bindings are `_mca` files. Now that `_mca` resolves to the
`MacroAmbient` suffix role, those 506 report a suffix disagreement against Super's
`Macro` slot where previously an unrecognised suffix stayed silent. That is the design
working — a disagreement is reported, never used to reroute — but it is a real jump in
the corpus disagreement count and should not be mistaken for a regression. Whether
Super's Stage3 can carry MCA semantics is a separate question with its own evidence
requirement.

## Suggested order

1. ~~`Skin`~~ — **done**, same day. Stage1 normal 347/347, Stage2 MC 346/347, Stage3
   `_co` base colour 340/347, Stage4 AS 347/347, Stage5 SMDI 347/347, Stage6
   `fresnel(...)` 347/347, Stage0 absent 347/347. Stage7 left unmapped: both its 2
   occurrences are Super's `env_co.paa`, and 2 materials do not describe a family.
2. `NormalMapSpecularDIMap` and the small `NormalMap*` family cluster — ~220 together,
   all the same two-or-three stage shape.
3. `Multi` — the largest object gap, but blocked on a layered-material design decision.
4. `TerrainSNX` / `TerrainX` — only once it is settled whether object material
   translation is the right owner for them at all.
