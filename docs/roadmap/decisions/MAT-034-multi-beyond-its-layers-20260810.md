# MAT-034 — Multi has six more stages than the census looked at

- **Ticket:** MAT-034
- **Date:** 2026-08-10
- **Closes:** the population `WGR_MATERIAL_SPEC_UNMAPPED` was left covering after MAT-033

## What the earlier census missed

MAT-030 measured `Multi` as far as Stage8 and concluded it was a four-layer material
with per-layer detail+specular beyond the mask. That was right as far as it went, and
it stopped three stages too early. Re-measured over all **1,628** Multi materials:

| stage | content | n |
|---|---|---|
| 0–3 | four colour layers (`_co`) | 1624 / 1628 / 1628 / 1628 |
| 4 | the blend mask (`_mask` 1576) | 1628 |
| 5–8 | per-layer `_dtsmdi` | 1628 each |
| **9** | **macro** — `_mc` 1168, procedural mc 443 | **1626** |
| **10** | **ambient shadow** — `_ads` 808, `_as` 388, `_adshq` 274 | **1628** |
| **11** | **normal map** — `_nohq` 1486, procedural nohq 141 | **1628** |

Stages 9–11 are **not layered**. They are per-surface exactly as Super's equivalents
are, so they raise no blending question at all: a Multi material has a normal map, a
macro and an ambient-shadow map like anything else, and this build was binding none
of them. That is 1,628 materials — the second largest object family in Arma 3 —
without a normal map, for want of looking past Stage8.

## `_dtsmdi` earns the SpecularDetail slot on measurement

MAT-030 left Stages 5–8 unmapped because splitting a combined detail+specular texture
would be a guess. It turns out no split is needed. Decoding **72** `_dtsmdi` textures:

- **A** constant 255 in **72/72**
- **R** centred on 128 in **64/72** (mean of means 135) — a detail multiplier around 0.5
- **G** varies in **72/72**, floors at 0 in **70/72** (mean 27)
- **B** varies in **58/72** (mean 140)

That is SMDI's channel assignment exactly — G the specular level, B gloss — with the
detail occupying R, which on a plain SMDI is dead (constant 255, measured n=40). So a
consumer reading a `_dtsmdi` as SMDI reads the right channels and simply does not
sample the detail half. No new channel map, no guess.

**Stage5 binds, paired with Stage0.** Layer 0's colour already stands in for the whole
surface; layer 0's detail+specular is its match. Binding layer 0's colour against a
different layer's specular would be worse than binding neither.

## Result on Stratis

Whole-world object census, 436 materials reporting a `SpecularDetail` outcome:

| | after MAT-033 | after this |
|---|---:|---:|
| specular map bound | 206 | **320** |
| `Multi` bound | 0 of 114 | **114 of 114** |
| **bright constant (≥0.5) and still no map** | **90** | **none** |

`WGR_MATERIAL_SPEC_UNMAPPED` now has an **empty population on Stratis**. The 116
materials still reporting no specular map are `TreeAdv` 45, `Normal` 42,
`TreeAdvTrunk` 25 and `TreeAdvTrans` 4 — none of which carries a bright constant, so
the approximation costs them nothing. It stays in the code because another world may
present the case; it is no longer doing any work here.

Albedo coverage is unchanged (293 with no RVMAT albedo, 291 of them falling back to a
working face texture, 2 unresolvable).

## Two corrections this forced

**`_ads` was an unrecognised suffix.** It is the majority spelling in this family — 808
of 1,628 Stage10 bindings against 388 `_as` — so the second witness stayed silent on
the commonest form of the commonest layered material. Now recognised.

**And `_ads` is not `_as`.** Measured over 47: R constant 255 in 46, A in 47/47, but
**B varies in 47/47** as well as G, and B's mean tracks G's within 2 in only 27 of 47 —
so B is a second quantity, not a copy. The `AmbientShadow` channel map declares B
unused, which is measured true for `_as` and false for `_ads`. Nothing samples the slot
yet so nothing is wrong in effect, but `unused` means "measured dead" and this is
recorded rather than left to be discovered by whoever first samples it.

## Still not done

The masked four-layer blend itself. Stages 1–3 and 6–8 remain unmapped: a second,
third and fourth colour layer have no representation in this slot set, and the Stage4
mask is preserved but sampled by nothing. That is the real Multi feature and it needs
the layered-material design decision MAT-030 asked for — this ticket just stops the
family being wrong about everything that is *not* layered.
