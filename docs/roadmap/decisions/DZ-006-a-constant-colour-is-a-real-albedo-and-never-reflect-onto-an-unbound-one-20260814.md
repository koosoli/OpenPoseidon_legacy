# DZ-006 — A procedural constant is a real albedo, and nothing may reflect onto one that failed to bind

**Date:** 2026-08-14
**Status:** Both changes implemented and measured. **Chernarus's white ponds remain unexplained** — the
leading hypothesis is recorded here with the one-run test that would settle it.
**Follows:** [DZ-003](DZ-003-the-water-renderer-is-one-global-plane-so-reflection-belongs-in-the-model-shader-20260814.md).

## 1. A procedural constant colour is a real albedo

DayZ water authors **no albedo texture**; its whole appearance is the `.emat`'s `Color` constant, which
[DZ-002 §4](DZ-002-dayz-authors-river-flow-in-a-texture-not-the-placement-20260814.md) emits as Real
Virtuality's own notation for a flat-colour stage, `#(argb,8,8,3)color(r,g,b,a,CO)`. `DrawSectionTL`
refused those by construction: its Stage0 bind was guarded

```cpp
base && !base->texture.isProcedural && !base->texture.path.empty()
```

and a procedural stage has no path *by design* — it is generated from its name. So the constant never
reached the GPU on that path and the surface fell back to white.

It now binds, and the retained path was brought into step so the two agree.

**`CalmWater` is a measured exception to MAT-045's face-texture precedence.** Normally a section's own
face texture wins over a material constant. Every DayZ water p3d on both worlds names the same face
texture — `dz\surfaces\data\roadway\water_ext.paa`, a roadway **surface-type marker** whose siblings are
`clean_water_ext.paa` and `still_water_ext.paa`. It is 128×128 DXT1 at mean RGB **(42, 49, 86)**, four
times brighter than the authored colour, and carries no per-surface information whatsoever. For these
materials the constant is the only real albedo, so it takes precedence.

## 2. Never add a reflection to an albedo that failed to bind

This is a **safety property**, not a tweak. Reflecting onto an unbound white fallback produces a
confidently wrong bright answer; failing back to the previous dull look is correct. The reflective bit
is therefore no longer set when the albedo did not bind (`d.texture_id` / `mat.texture_id == 0`), on
both paths, with a `LOG_WARN` naming the material so the condition is visible rather than silent.

## 3. Chernarus's white ponds — what they are NOT

ChernarusPlus's ponds render as saturated white blobs. An earlier draft of the handover blamed the
DZ-003 reflection. **That was wrong**, and the refutation is direct:

* With **both** `WGR_ENV_REFLECTION=0` **and** `WGR_WATER_RIPPLE=0`, the ponds are **still white**
  (`cherna-both-off.png`). It is a **pre-existing defect** that DZ-003 did not introduce.
* It is **not** the unbound albedo of §1 either. With the constant binding — log confirms
  `Stage0 BaseColour resolved path=#(argb,8,8,3)color(0.0392,0.0549,0.0667,0.3725,CO)` — the pond pixels
  barely moved: bright-region mean RGB **207/233/248 → 205/233/248**. An albedo of 0.039 cannot produce
  205 under any lighting.
* They *are* DayZ's pond models, not the ocean plane: 71 `dz\water\ponds\moss\*` placements inside the
  captured view box.

### The leading hypothesis, unproven

`TranslatedMaterial` defaults `specular = {1,1,1,1}` with `specularPower = 1.0` — a full-white specular
lobe with an exponent of 1, i.e. near-full-screen coverage on a flat up-facing quad. The retained path
damps exactly this through `EngineWgpu::MaterialSpecularScale` (0.25 for a schema-known material with no
specular map); **the per-draw path has no equivalent** — `MaterialSpecularScale` is called from exactly
one site, inside `RegisterGpuModel`. **Chernarus's ponds are on the per-draw path and Enoch's are
retained**, which matches the symptom precisely and explains why only Chernarus shows it.

If confirmed the fix is a *parity* fix — damp unmapped specular on the per-draw path the way the
retained path already does — but it would change lighting for every per-draw section, so it needs
measuring before shipping.

**A test that does not discriminate, so nobody repeats it:** capturing at night. Both albedo and
specular scale with incident light, so at hour 0 the whole scene goes black (brightest 3% = 0.3,
terrain median = 0.0) and the comparison says nothing. A *low but non-zero* sun, or simply applying the
damping behind a flag and measuring, is what separates the two.
