# PoseidonWGPU Roadmap Amendment — Height / Parallax / POM

**Date:** 2026-08-09
**Applies to:** PoseidonWGPU Modernisation Roadmap v4.5.3 and all later live repository state
**Status:** Additive amendment — does **not** replace the roadmap, execution overlay, status ledger, or capability matrix.

---

## Purpose

This amendment adds explicit support for **height/parallax material semantics**, including Arma 3 `_NOPX` compatibility and a modern **Parallax Occlusion Mapping (POM)** path.

It is intentionally additive so that current AI-agent progress, ledger state, evidence, execution ownership, and capability claims are not overwritten or rolled back.

The **live repository status ledger remains authoritative** for ticket progress.

This amendment must not change the current COMPAT-C1 / material execution order unless explicitly promoted by the execution overlay or owner.

---

# MAT-PAR-010 — Height / Parallax Material Support

**Role:** SUGGESTED
**Milestone role:** NON_BLOCKING_VALIDATION / later material capability
**Ticket class:** RUNTIME_INFRA / LOCAL_COSMETIC
**Default scheduling state:** HOLD until the core A2/A3 material stack is integrated and visually validated.

## Goal

Support source height/parallax semantics from Arma-family materials and provide a modern WGPU implementation using **Parallax Occlusion Mapping (POM)** where appropriate.

The primary compatibility target is Arma 3 height/parallax data such as `_NOPX`, while preserving source semantics rather than flattening them into an unrelated material model.

POM is a **visual material effect only**. It must not silently alter collision, AI geometry, projectile collision, navigation, authoritative terrain height, or physics.

---

## Required source semantics

The import/material pipeline must preserve and expose:

- source texture path;
- source material/RVMAT stage;
- shader schema role;
- UV source / TexGen;
- normal-map channels;
- height channel;
- height scale / bias where source semantics provide them;
- whether the source requests or implies parallax behavior;
- unknown or unsupported semantics without guessing.

Do not infer height/parallax merely from a filename when the shader schema or material structure provides stronger evidence.

---

## Compatibility requirements

### Arma 3

Investigate and support `_NOPX` and any associated RVMAT/shader semantics using:

1. official BI documentation;
2. official Arma 3 Samples;
3. controlled MLOD → BI Binarize → ODOL differential fixtures;
4. synthetic fixtures;
5. wider owner-local corpus validation.

Do not derive the implementation from a single retail fixture.

### Other generations

Preserve relevant A1/A2/OA height/parallax semantics where encountered, but do not claim cross-generation support until evidence exists for the specific source shader/material family.

---

## WGPU implementation

Provide at least two paths:

### 1. Normal-only fallback

When parallax is disabled, unsupported, too distant, or outside budget:

- retain correct normal mapping;
- do not lose the source material;
- fall back cleanly without changing unrelated material contributions.

### 2. Parallax Occlusion Mapping

Preferred higher-quality path.

Requirements:

- view-dependent UV displacement;
- bounded ray/sample count;
- configurable quality tiers;
- distance-based fade / disable threshold;
- grazing-angle safeguards;
- stable behavior under camera movement;
- no camera-relative randomization;
- no temporal shimmer introduced by unstable sampling;
- preserve correct UV source;
- respect alpha/cutout/material section behavior;
- avoid sampling undefined height data.

Optional later improvements may include:
- adaptive step count;
- refinement/binary search after coarse stepping;
- self-shadow approximation where performance allows.

---

## Performance requirements

POM must be treated as an optional material cost, not a universal baseline.

Measure at minimum:

- GPU cost per quality tier;
- effect of screen coverage;
- texture sample count;
- impact on vegetation/terrain-heavy scenes if applicable;
- near/mid/far behavior.

Prefer:
- high quality close to camera;
- reduced quality at medium distance;
- normal-only fallback at distance.

The implementation must fit within the renderer budget framework (`RND-000`) and must not silently become an unbounded per-pixel cost.

---

## Material Debug integration

The Material Debug Dev Tools page should eventually expose:

- **Disable Parallax**
- **POM Quality**
- **Visualize Height**
- source height texture path
- height channel
- UV source
- parallax enabled/disabled reason
- fallback reason

Useful capture modes:

1. Base Color Only
2. Normal Only / Parallax Off
3. POM Enabled
4. Height Visualization

The same debug state should remain usable by automated capture/testing where practical.

---

## Validation methodology

Use controlled differential fixtures before broad corpus claims.

Recommended sequence:

1. start from an official BI sample MLOD;
2. produce a baseline ODOL using BI Binarize;
3. create a controlled variant changing only the relevant height/parallax semantic;
4. binarize again;
5. compare semantic and structural differences;
6. implement parser/material translation;
7. verify with a synthetic test;
8. validate against multiple independent official-sample-derived fixtures;
9. validate against wider owner-local content;
10. render through the production WGPU path.

Do not rely only on raw byte diffs: Binarize may reorder or regenerate derived structures. Compare canonical semantics as well as changed binary ranges.

---

## Acceptance criteria

MAT-PAR-010 may be promoted to **VALIDATED** only when:

- [ ] a supported source material with real height/parallax semantics is identified;
- [ ] its source semantics are preserved in the canonical material representation;
- [ ] correct height channel/UV source are evidence-backed;
- [ ] normal-only fallback works;
- [ ] POM renders through the real WGPU production material path;
- [ ] parallax on/off produces a controlled visible difference;
- [ ] height visualization is available in Material Debug or equivalent automated diagnostics;
- [ ] distance fade/fallback is implemented;
- [ ] grazing-angle behavior is bounded;
- [ ] performance is measured;
- [ ] no unrelated alpha/material regressions are introduced;
- [ ] capability claims identify the tested shader/material family and fixture corpus.

---

## Non-goals

MAT-PAR-010 does **not** require:

- true geometric displacement;
- hardware tessellation;
- collision displacement;
- terrain-authority changes;
- AI/navmesh changes;
- projectile collision changes;
- arbitrary model deformation;
- a second renderer backend.

---

# Future investigation — True Displacement

True geometry displacement/tessellation is **not currently committed roadmap work**.

If investigated later, it should be a separate ticket and must address:

- WGPU-compatible implementation strategy;
- geometry generation/subdivision;
- LOD stability;
- collision/simulation divergence;
- terrain integration;
- multiplayer determinism where gameplay-relevant;
- performance and memory budgets.

Until then, **normal mapping + optional POM is the preferred modern material-depth solution**.

---

# Scheduling / dependency note

MAT-PAR-010 must **not interrupt the current A2/A3 material integration work**.

Current priority remains:

1. production A3 material/RVMAT integration;
2. normal-map validation;
3. Detail;
4. Macro;
5. Ambient Shadow;
6. SMDI/specular-gloss;
7. Fresnel/Environment;
8. then height/parallax/POM unless evidence or the execution overlay explicitly reprioritizes it.

This amendment does not authorize work merely by existing. The execution overlay / owner must promote it when appropriate.

---

# State-preservation rule

When this amendment is later folded into a consolidated roadmap release:

- start from the **current live repository state**, not an old package snapshot;
- preserve all current status-ledger ticket states;
- preserve evidence hashes and decision records;
- preserve capability-matrix claims and denominators;
- preserve execution ownership/scheduling;
- do not recreate the live ledger from an old seed;
- regenerate hashes/checksums only after merging the amendment forward.
