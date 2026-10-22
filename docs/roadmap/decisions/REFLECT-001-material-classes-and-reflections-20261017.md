# REFLECT-001 — Material classes and reflections for vehicles (phase A findings, phase B–D proposal)

Status: PROPOSED (2026-10-17). Owner decision pending. Branch `MrDudeX-reflect` from
`feat/arma-original-ui` @ `2f63d5dd`. Nothing in the engine is changed by this document.

## What exists (read from the code, verified at 458bd70 and present on feat/arma-original-ui)

- Vehicles are never in the retained GPU path (`EngineWgpu.cpp` admits `Static()` only), so all
  vehicle shading happens in `gfx3d/shader3d.wgsl`; `gpu_driven.wgsl` must be kept in step.
- Per draw: sun-only Blinn-Phong (`Material.specular`, w = exponent) plus the SH-9 sky sheen
  (`frame.wgsl::sky_specular_split`, F0 0.04). Its own comment: a sharp lobe "is not attempted".
- `sky_env` (256x128 equirect, 1 mip, rebaked per frame, sky only) is bound to both object
  pipelines; `MATFLAG_REFLECTIVE` uses it only for DayZ CalmWater and glass (Schlick F0 0.02).
- CWA gloss source: `CfgTextureToMaterial` -> `CfgMaterials`. In stock CWA that table names
  only jeep/scud glass (`#SpecularGlass`, specular 0.8, power 5), Mutt and Cobra metal and rifle
  metal. Every other vehicle texture has no specular at all.
- RVMAT: all stages are parsed; colour constants, specular, `_nohq` and (retained path only)
  SMDI green are used. The Fresnel stage, the env-map stage and SMDI blue are parsed and unused.
- `WgrDraw3D` has an append-only precedent (`mat_local_specular`, 272 -> 288 bytes, covered by
  the layout handshake). Roadmap item LIT-040 (IBL, prefiltered env, roughness/metal) is SUGGESTED.

## Inventory (own tool, paths and classes only; `_OpenPoseidon-Reflect\inventory` on the test PC)

| Source | What it gives | Caveat |
| --- | --- | --- |
| Stock CWA | texture names only | atlases mix paint, chrome, rubber in one texture: class per texture, masks later |
| Arma 1 | `NormalMapSpecularMap` + SMDI, glass `Glass` shader + env map, chrome via `chrome_enviro.rvmat` | paint has no Fresnel stage |
| Arma 2/3 | `Super` shader with `fresnel(N,K)`, env map, SMDI | N,K are artistic: military bodies come out at F0 ~0.71 by the physical formula, so F0 is not taken literally; specular[] + SMDI carry the intent |
| Own addons (Peugeot 406) | paint finish per colour (Uni / Metallic / Perl) from the stringtable, parts by texture | `tex.paa` is a mixed atlas |

## Proposal (switch `WGR_REFLECT`, default 0 = today's image bit for bit)

B. Material data: class table file + chain own > RVMAT > CfgMaterials > rules > default_matt;
   two appended `WgrVec4` lanes in `WgrDraw3D` (roughness, F0/metal, clear coat, flags/class);
   debug views class / roughness / F0.
C. GGX sun + local lights and a Filament clear-coat layer for classified draws only.
D. Prefiltered mip chain of `sky_env` (GGX importance samples as in `ground_wet.wgsl`), DFG LUT,
   ground below the horizon instead of horizon colour; glass F0 0.02 -> 0.04.
E. Only if measured: object SSR from `rain_water_reflections.wgsl` or one dynamic probe.

## Owner decisions (2026-10-17 23:08)

1. Own addons declare materials in `CfgOpMaterials` (ignored by CWA 1.99). Stock data gets a
   general part-based reading so typical parts reflect without declarations: glass panes,
   mirrors, chrome trim and fittings; headlights by era (old: glass cover; modern: plastic
   cover over a chromed reflector).
2. Military matt / civil gloss as the base rule, with the same part logic on top (a military
   truck still has reflective glass, mirrors and lamps).
3. On by default after acceptance, with quality levels in the options menu.
4. GL33: gloss strength plus environment reflection as far as GL33 allows.
5. Peugeot 406: keep the v4 paints; new paint textures only if needed, first on one colour,
   then all 21 once the reflection is right.

## Phase B (implemented, off by default)

- `engine/WgpuRenderer/ReflectMaterial.hpp`: pure rule table (no engine types), tested by
  `tests/unit/engine/WgpuRenderer/test_reflect_material.cpp` (`scripts/Test-ReflectMaterial.ps1`).
  Chain: CfgOpMaterials > RVMAT (Glass shader, `fresnelGlass`, env map) > part names
  (glass/sklo, mirror/zrcatko, chrome, light/svetl by era, tyre/guma, interior) > body
  (military `PaintMatt`, civil `PaintGloss`, civil + env map `PaintMetallic`) > Blinn default.
- `TexMaterial` keeps `_pixelShaderId`, `_fresnelStage`, `_hasEnvMap` from the RVMAT.
- `EngineWgpuReflect.cpp` gathers owner (Transport, typical side), texture and material per
  section, memoised per thread, and reads `CfgOpMaterials` once (format in the file header).
- ABI: `WgrDraw3D` 288 -> 320 (`mat_reflect0` roughness/F0/metal/coat, `mat_reflect1`
  class/coat roughness/source/debug view); `MaterialUbo` 128 -> 160; layout hash covers it.
- Switches: `WGR_REFLECT=1` fills the lanes (nothing shades with them until phase C);
  `WGR_REFLECT_DEBUG=1..5` shows class / roughness / F0+metal / clear coat / source.
  Both unset: lanes zero, no shader path reads them -- the frame is unchanged.
- GL33: no change (owner asked 2026-10-17 whether GL33 should be retired; see the reply in
  the session -- decision 4 is on hold until that is settled).

## Rebased onto the WGPU migration line (2026-10-22)

`MrDudeX-reflect` now sits on `MrDudeX-wgpu-aktuell` (2ff794f + docs 995eb33f), the build the
owner's game folder runs; the previous base (`feat/arma-original-ui` d707fd10) is kept locally
as `backup/reflect-on-arma-ui-8d7d42e8`. WGPU-033's draw batching keeps the material per draw
slot, so the two appended lanes travel with every batched draw unchanged.

## Phase C (implemented, behind WGR_REFLECT=1)

- `shading.wgsl`: `ShadeMaterial` carries the lanes (`refl_a`, `refl_b`; zero on the retained
  path). Classified surfaces get GGX base + clear-coat sun lobes instead of Blinn, lose the SH
  sky sheen (the env map replaces it) and, when metal, their diffuse body.
- `shader3d.wgsl`: sky env map in the reflection direction, five-tap roughness cone, horizon
  and below-horizon dimming (street, not sky), GTAO and cave occlusion; panes in the blend
  pipeline composite the reflection over the film (RFG-137 form). Skipped where DZ-003
  already mirrors.
- Lamps: chrome bowl (metal base) under a glass/plastic cover (clear coat).
- Handed over from the WGPU project: `setObjectMaterial` / `hiddenSelectionsMaterials` are Arma 3
  stubs (`GameStateExtA3Stubs.cpp`). Hidden selections already switch textures per instance
  (`AnimationSection::SetTexture` on the shared shape each frame); materials need the same plus
  a restore of the section's own material for instances without an override. Planned as B2.

Risks: baked highlights in old textures (double highlight), atlases, auto-exposure on bright
ground (the Intro desert overexposes front-3/4 shots), GL33 gets roughness only.
