# DZ-005 — DayZ's water quads have no UVs at all, so the ripple is sampled in world space

**Date:** 2026-08-14
**Status:** Ripple and pond drift implemented and measured. **River flow-map flow is NOT implemented** — see the end.
**Follows:** [DZ-003](DZ-003-the-water-renderer-is-one-global-plane-so-reflection-belongs-in-the-model-shader-20260814.md).

## The problem

After DZ-003 DayZ water reflected the sky but read as **glass**: mirror-flat, no ripple. The normal map
was bound and fed the shading normal, and produced no visible perturbation.

The suspicion going in was that the river meshes' UVs are too anisotropic to be useful — u runs along
the river at 20,740 m per unit (DZ-002 §1), which on a 1024-wide normal map is ~1.7 texels across a
whole 50 m quad. That held, and the reality was worse.

## Three separate faults, all measured

1. **The pond meshes have no UVs.** All **185 Enoch pond meshes carry UV `(0.000, 0.000)` at every
   vertex** — not anisotropic, *degenerate*. `duv_dx == duv_dy == 0`, so the cotangent frame in both
   object shaders hit `frame_scale2 == 0` and **the normal map was never applied at all**. Chernarus's
   `dz\water\ponds\moss\*.p3d` are identical in this respect.
2. **The river meshes' UVs are near-useless for this**, as suspected: `enoch_river_325` spans u
   0.498–0.504 over 50 m against v 0.082–1.000.
3. **The decode was wrong even where a frame existed.** `enoch_pond_nohq.edds` and
   `enoch_river_nohq.edds` are **byte-identical DXT1, 1024×1024, with no alpha** — alpha is a constant
   255, and R/G/B carry a unit normal (mean `|2v−1|` = 0.928, R/G centred on 128 ± 28/255). The shaders
   decode the OFP NOHQ convention `(a, g)`, which on these yields a constant `(+1.00, −0.02)`: a normal
   tipped 90°, identically everywhere.

## Why world space is the right frame, and the material says so

Both water `.emat` files declare **`NormalMapping 0.1`** — a *world-space* frequency, one tile per
10 m. **That is why the quads never needed real UVs**: DayZ's own water shader never used them either.
The degenerate UVs are not a data defect, they are a consequence of how Enfusion samples this material.

So the ripple samples the normal map at `world_xz / period` with `textureSampleGrad` (gradients from
the world-position derivatives already taken at the top of the fragment — legal inside the branch, and
correct mips so distant river does not alias), `samplers[0]` forced to linear+repeat (an authored clamp
would smear one texel across a whole lake), decoded as `s.rg`, and turned into a normal by
`water.wgsl`'s own convention `normalize(vec3(-sx, 1, -sy))`. Two octaves at 10 m and 3.3 m, powers
1.55 / 0.80 — the authored `NormalPower`.

## Measured (Enoch pond, `WGR_WATER_RIPPLE` 0→1, same binary)

| region | σ | 8-px high-frequency σ |
|---|---|---|
| pond | 5.848 → 2.678 | **0.932 → 1.561 (+67%)** |
| pond, far | 4.599 → 5.031 | **1.291 → 2.356 (+83%)** |
| control, dark | 0.806 → 0.702 | 0.346 → 0.369 |
| control, terrain | 4.022 → 4.116 | 2.460 → 2.404 |

Overall σ *drops* on the near pond because a smooth mirror gradient is replaced by ripple; **the
high-frequency figure is the metric**, and it nearly doubles. **0 uncaptured wgpu errors.**

### Pond drift

Two-octave time drift (0.12 / 0.075 m/s in different directions) from the engine's own `Glob.time` via
`WgrWaterParams.time`, so it pauses when the simulation does. Two captures 300 frames apart, fixed
camera:

| region | rms \|Δ\| | fraction > 2/255 |
|---|---|---|
| pond | 2.139 | 27.0% |
| pond, far | 2.828 | 20.7% |
| control, dark | 0.043 | **0.000%** |
| control, terrain | 0.140 | **0.000%** |

σ and high-frequency σ are unchanged between the two frames: the pattern **moved without changing
character**, which is what animation should look like and a flicker should not. Cross-correlation gives
a coherent shift of dx −9, dy +5 px (correlation 0.74 against 0.61 at zero shift), matching the
dominant octave's authored direction.

## What is NOT done

**River flow-map flow.** `WaterStreamMap` has no handling anywhere in C++ — a grep returns nothing. It
needs a new `MaterialSlot`, an `EmatMaterialAdapter` mapping, an `EngineWgpu` resolve and a new FFI
entry point, across four files other agents were actively editing. Enoch's rivers currently ripple like
ponds and do not move downstream.

The plumbing is already in place and unused, so the remaining shader work is roughly ten lines once the
texture arrives: `frame.wateranim.z/.w` are reserved for `StreamSpeedInfl` and the flow map's bindless
slot, `Renderer::water_flow_slot` / `water_flow_strength` exist and are 0, `Gfx3d::set_water_anim` takes
them, and `water_ripple_normal` already accepts per-octave drift vectors. Everything needed to *use* a
flow field is measured in [DZ-002 §1](DZ-002-dayz-authors-river-flow-in-a-texture-not-the-placement-20260814.md).

**No ABI struct grew**; the only layout change is +16 B on the Rust-side frame UBO.

## Also not claimed

* **Ripple periods and powers are shader constants, not per-material.** Fine today — all DayZ water
  agrees on them — and wrong the moment a second game's water arrives.
* The Enoch test scene is very dark overall (mean ~24/255 at hour 11 on `devtest-dawn`), which makes
  eyeballing this camera misleading. Judge it by the high-frequency figures, not by the screenshot.
