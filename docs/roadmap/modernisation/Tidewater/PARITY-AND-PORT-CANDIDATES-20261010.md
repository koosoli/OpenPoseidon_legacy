# Tidewater parity, Tidewater PRs, particle fluids and Fire Pro: what to port

Date: 2026-10-10. Read-only research. Nothing in the repo was edited, built or deployed, and the game was not launched.
Our baseline: worktree `whole-world-dec`, branch `codex/whole-world-horizon-dec-20261010` @ `d29f57377`.
External baselines:
- `dgreenheck/tidewater` `main` @ `4811ba48d795197de5621985f404e765c0b7c0ef` (2026-09-25). This is still the head of `main`, and it is the exact commit our `water_tw/` port was taken from (`CREDITS.md` §Tidewater). **Upstream `main` has nothing new to port.** The only new material is in the pull requests (§3).
- `dgreenheck/threejs-particle-fluids` `main` (last pushed 2026-09-28).
- `dgreenheck/threejs-fire-pro` `main` @ `c284f0b1` (2026-10-06). It has 5 commits; the first is "Initial release".

Earlier work, read first and not repeated here:
- `CREDITS.md` §Tidewater: the file-by-file port table.
- `docs/tw-water-w7-report.md` (W7 perf/parity) and `docs/tw-water-w8-w12-report.md` (W8–W12d).
- `docs/roadmap/decisions/TW-WATER-*.md`.
- `docs/roadmap/decisions/TIDEWATER-STREAMING-REVIEW-20260926.md` (impostors, CDLOD ordering, pipeline warm-up).
- `docs/roadmap/WATER-REALISM-LOW-COST-IMPLEMENTATION-PLAN.md` §3.6 and WRL-006b. These are the buoyancy constraints: queries must work headless, no GPU readback, no kinematic snap.
- `docs/roadmap/modernisation/OpenPoseidon_WGPU_Ocean_and_Shore_Surf_Plan.md`.

---

## 1. Licences (first)

| Repo | Licence | Evidence | Can code or shaders go into GPL-3.0 OpenPoseidon? |
| --- | --- | --- | --- |
| tidewater | **MIT**, "Copyright (c) 2026 DRG Software Solutions LLC" | GitHub API `license.spdx_id = MIT`; `LICENSE` at 4811ba48; README footer. Third-party assets (CC0, MIT, OFL/Apache) are listed in its `CREDITS.md`. | **Yes, with attribution.** MIT is GPL-compatible. Keep the MIT notice; it is already reproduced in our `CREDITS.md`. **Code only, no assets:** audio, GLB models, textures and cloud noise each have their own licence. |
| threejs-particle-fluids | **MIT**, "Copyright (c) 2026 Daniel Greenheck" | API `spdx_id = MIT`; `LICENSE`; README §License. The bundled Kenney and Poly Haven models are CC0, and the Stanford bunny is credited separately (`THIRD_PARTY_NOTICES.md`). | **Yes, with attribution** (code only). |
| threejs-fire-pro | **MIT**, "Copyright (c) 2026 Daniel Greenheck" | API `spdx_id = MIT`; `LICENSE` file; README §License reads "Fire Pro is released under the MIT License". Fonts, models and textures are in `THIRD_PARTY_NOTICES.md` (12.8 KB). | **Yes, with attribution.** Despite the "Pro" name it is **not** a paid or closed product: the full source is public, including the WGSL in `src/engine/shaders/*.wgsl`. I found no store page or paid tier. Do not copy the editor's scene assets. |

Two cautions:
- **threejs-water-pro is a different matter.** It is a separate product by the same author, and Tidewater's `BoatSpray.js` says its contact model is "after threejs-water-pro". We already use that file under Tidewater's MIT grant, on the owner's decision (`CREDITS.md` line 139). Never open or consult water-pro itself. Nothing in this report depends on it.
- **PR code belongs to the PR authors.** None of the 11 Tidewater PRs was merged (§3). Under GitHub's ToS inbound=outbound clause a contribution to an MIT repo is arguably MIT, but nobody has accepted it.
  - Rule: re-implement ideas from PRs. Copy only small pieces, and name the PR author in `CREDITS.md`.
  - PR #15 also carries third-party models and audio (Rocketbox crowd, Kenney, recorded engines). Never copy those.

**Recommendation:** all three repos are portable (MIT). Add a `CREDITS.md` section for each new source the same way Tidewater's was added. Effort: S. Target: `CREDITS.md`, `docs/roadmap/publishing/public-files/CREDITS.md`.

---

## 2. Tidewater feature parity (water, ocean, shore, underwater, foam, caustics, buoyancy, boat)

Legend: **Yes** = ported or equivalent; **Partial**; **No**. Paths are relative to `engine/WgpuRenderer/rust/src/` unless stated.

| Tidewater feature (file @4811ba48) | OP | Where / note |
| --- | --- | --- |
| 4-cascade FFT ocean, JONSWAP swell + wind sea (`ocean/OceanFFT.js`) | Yes | `water_tw/fft.rs` |
| CDLOD sea mesh to the horizon (`core/CDLOD.js`) | Yes | `water_tw/cdlod.rs` (16 levels, W9) |
| Sea detail, foam texture, whitecaps, wind streaks, gust slicks (`SeaDetail.js`, `FoamTexture.js`, `WaterMaterial.js`) | Yes | `water_tw/textures.rs`, `tw_water.wgsl:474-479` (generated procedurally, no TW textures) |
| Water shading: Fresnel, GGX, sky/cloud reflection, SSR (`WaterMaterial.js`) | Yes | `tw_water.wgsl` (`_waterSSR` l.616; OP adds the SSR altitude fade W9g) |
| Snell's window / TIR seen from below | Yes | `tw_water.wgsl` (view side from `front_facing`, W9d) |
| **RefractionPass**: a separate below-water scene pass clipped at sea level + guard band (`ocean/RefractionPass.js`) | **No** | `tw_water.wgsl:13-15, 869-876` still falls back to the opaque copy (TW's `!found` path). Visible as smeared sea bed beside piers, hulls and rails. |
| Hull masking: water removed inside an open hull (`WaterMaterial.js`) | Partial | `tw_wake.wgsl:64-69, 96-102` uses the hull mask only to suppress wake normals and heights; the water surface is not cut. |
| Depth-aware shore waves and breaking (`ShoreWaves.js`) | Yes | `water_tw/tw_shore.wgsl` |
| Shore field (`world/ShoreField.js`) | Yes | `water_tw/shore_field.rs` |
| Swash sim and the thin swash film, including the last two TW commits (backwash meander, thin-water sand) | Yes | `water_tw/shore_sim.rs`, `tw_sim*.wgsl`, `tw_water.wgsl:724-733` |
| Breaker lips (`Breakers.js`) | Yes | `water_tw/breakers.rs`, `tw_lip.wgsl` (OP-traced shorelines) |
| Surf foam and lace (`SurfFoam.js`) | Yes | `water_tw/lace.rs`, `tw_surf_foam.wgsl` |
| Spray particles: breakers, sprites (`fx/Spray.js`) | Yes | `water_tw/spray.rs`, `tw_spray_*.wgsl` (own pass after clouds, W12a) |
| Boat wake sim with Kelvin shaping (`WakeSim.js`) | Yes | `water_tw/wake.rs`, `tw_wake*.wgsl` (2 boats) |
| Whale wake and breach water (`WhaleWater.js`) | No | Not relevant: no whales |
| Bow spray (`player/BoatSpray.js`) | Yes | `water_tw/boat_spray.rs` |
| Caustics on the sea bed and in the water (`Caustics.js`) | Yes | `water_tw/caustics.rs`, `tw_caustics*.wgsl` |
| **UnderwaterLighting**: per-material sun/ambient attenuation from camera-centred baked maps (0.25 m over 128 m, 2 m over 1 km) | Partial | Applied **in post** to the opaque copy: `tw_water.wgsl:908-919` (W3f) and `tw_underwater.wgsl` header. Our terrain and object shaders are not hooked, so per-material caustic modulation is approximate. |
| Underwater composite: lens medium, Beer-Lambert, single scatter, meniscus | Yes | `water_tw/underwater.rs`, `tw_underwater.wgsl` (W5a, W9a probe, W9e, W12d mesh-exact waterline) |
| Caustic light shafts + diver's torch in-scatter | Yes | `tw_underwater.wgsl` (W5b full-res fewer steps; W5e torch from OP spot lights) |
| Marine snow: suspended specks, swell orbital motion, torch-lit (`fx/MarineSnow.js`) | **No** | No match for `marine.?snow` |
| Lens droplets after surfacing (`post/LensDroplets.js`) | **No** | No match for a lens-droplet post pass |
| Wet sand and foam residue on the beach (`App.js` `terrainWetness`, `Terrain.js`) | Yes | `terrain/shore_wet.rs`, `terrain/terrain.wgsl` |
| GPU water query with async readback for physics, latency-compensated (`WaterQuery.js`) | Partial | Camera, underwater gate and swimming use the probe readback (`water_tw/probe.rs`, `mod.rs:309, 502`). **Boats use the CPU 6-component sea** (`engine/Poseidon/World/Terrain/WaterSurfaceQuery.cpp:21-66`), which matches Tidewater's amplitude but not its crests. |
| **Rigid-body boat**: hull-patch buoyancy, added mass, resistance curve, keel lift, prop and rudder, trim, 120 Hz (`player/BoatController.js`) | **No** | `engine/Poseidon/World/Entities/Vehicles/Misc/Ship.cpp:612-889` is the 2001 point model with planing-gain patches (W11b–e). See §4b. |
| Boat vs pier/pile contact (`BoatController.contacts`) | Partial | Ship.cpp handles land contact only (`:772-830`); piers are generic object collision. |
| Loose floating objects | Partial | `engine/Poseidon/World/Physics/Box3DBackend.cpp:265-331`: AABB-fraction buoyancy plus implicit quadratic drag for probes |

Net: the **surface, shore, wake, spray, caustics and underwater-composite** parts of Tidewater are at parity. The real gaps are:
- RefractionPass;
- the hull cut-out;
- UnderwaterLighting as material hooks;
- marine snow and lens droplets (cheap atmosphere);
- the **boat physics**, which the owner parked; the W8–W12 report says "the owner wants the boat physics reworked to match Tidewater later".

**Recommendation (port code, MIT):**
1. RefractionPass. Effort: M. Targets: `water_tw/mod.rs`, `tw_water.wgsl`, a below-sea-level clip define in `gfx3d/shader3d.wgsl` / `terrain.wgsl`.
2. Marine snow. Effort: S. Target: new `water_tw/marine_snow.rs` + `.wgsl`.
3. Lens droplets. Effort: S. Targets: new post pass after `temporal.rs`, before `tonemap.rs`.
4. Hull cut-out. Effort: M. Targets: `tw_water.wgsl`, `wake.rs`.
5. Boat physics: see §4b.

---

## 3. Tidewater pull requests

Source: `https://api.github.com/repos/dgreenheck/tidewater/pulls?state=all`. There are 11 PRs and **none was merged** (`merged_at = null` on every closed one).

| # | State | Title | Useful to us? |
| --- | --- | --- | --- |
| [18](https://github.com/dgreenheck/tidewater/pull/18) | open | Fix WGSL and resource limits for integrated WebGPU adapters | **Yes** |
| [15](https://github.com/dgreenheck/tidewater/pull/15) | open | Island expansion: ferry, jetski, cars, townsfolk… (100+ files) | **Yes**, in parts (rendering section) |
| [10](https://github.com/dgreenheck/tidewater/pull/10) | closed | Fix player clipping, boat-pier collisions, stair traversal | **Yes** (boat contact) |
| [19](https://github.com/dgreenheck/tidewater/pull/19) | closed | "Claude/inspiring albattani" (Flannan/Hebrides terrain, StyleDirector, beacons; no description) | Marginal |
| [13](https://github.com/dgreenheck/tidewater/pull/13), [11](https://github.com/dgreenheck/tidewater/pull/11) | closed | Heli/jetski/boar, beach dog | No (game content) |
| [12](https://github.com/dgreenheck/tidewater/pull/12) | closed | Modern villas + pt-BR | No |
| [14](https://github.com/dgreenheck/tidewater/pull/14) | open | i18n + adapter name in the perf panel | Idea only (adapter in perf HUD) |
| [16](https://github.com/dgreenheck/tidewater/pull/16) | open | iOS WKWebView wrapper | No |
| [7](https://github.com/dgreenheck/tidewater/pull/7) | closed | "AAA ocean prompt" doc | No |
| [1](https://github.com/dgreenheck/tidewater/pull/1) | open | Pin vite version | No |

**Top 5 ideas, ranked:**

1. **PR #18: capability-tiered water/terrain shader paths for low-limit adapters.**
   - What it does:
     - Splits the FFT and mip generation into more dispatches to fit 16 KiB of workgroup storage.
     - Restricts binding visibility to the stages that use them.
     - Prunes unused bindings.
     - Adds a 16-sampled-texture water variant. That variant uses the env cube instead of the full sky/cloud shader, drops cloud-shadow modulation and textured lace, and keeps analytic foam and hull masking.
     - Adds a shader-composition test against real Dawn.
   - Why first: our `tw_water.wgsl` binds many textures (FFT array, shore, sim, lace, wake ×3, caustics, sky). Integrated GPUs and the AMD start-up crash (memory: `amd-vulkan-crash-gpu-driven-pipelines`) are already live problems, and a "tier" path is the only way Tidewater mode reaches them.
   - Re-implement. Effort: M.
   - Targets: `water_tw/mod.rs` (pipeline variants keyed on `device.limits()`), `water_tw/fft.rs` (dispatch split), `tw_water.wgsl` (`#ifdef`-style const branch), and our headless pipeline test (memory: `naga-tests-miss-resource-validity`).

2. **PR #15 "Water refraction": filtered behind-surface test and a shrinking-offset fallback.**
   - What it does: takes four depth taps weighted bilinearly for the "is the refracted sample behind the surface" test. On failure it tries shorter refraction offsets, down to straight-behind, before falling back to the opaque copy.
   - What it removes: the ragged fringe around moored hulls and dark dashes at drop-offs.
   - Our `tw_water.wgsl:871-875` does exactly the single-tap test, so we have the same fringe, and we lack RefractionPass, which makes the fallback matter even more.
   - Re-implement (about 20 lines). Effort: S. Target: `tw_water.wgsl` around l.869.

3. **PR #10: boat-vs-pier contact.**
   - What it does:
     - Subdivides the boat's movement, applies horizontal contact impulses against piles and decks, and leaves heave, pitch and roll to buoyancy.
     - Builds hull and cabin collision proxies for walking or swimming on board.
     - Derives trunk colliders from the tree-generation dimensions.
   - Why we want it: our ship's land contact pushes along the terrain normal (`Ship.cpp:772-790`) and has no notion of a pier. The split "contacts are horizontal, buoyancy owns the vertical" removes the "boat climbs the pier" failure, and it fits Box3D, where piers already exist as static convex pieces (`PhysicsBackend.hpp`).
   - Re-implement. Effort: M. Targets: `Ship.cpp` contact block, `World/Physics/Box3DBackend.cpp` (a query for static pieces near the hull outline).

4. **PR #15 rendering extras.**
   - Velocity coverage for thin blended particles, so spray and rooster tails do not ghost under TAA. Relevant to DLSS and our spray (`tw_spray.wgsl` writes no motion vectors).
   - A camera-cut reset of the temporal history on a 20 m jump or a teleport. Check `temporal.rs` / `dlss.rs` for an existing cut test.
   - The ferry as a WakeSim source with a frayed wake edge and foam compression (for our LST: `wake.rs` currently handles 2 boats).
   - Re-implement. Effort: S each. Targets: `water_tw/tw_spray.wgsl`, `temporal.rs`, `water_tw/wake.rs`.

5. **PR #19: far-shore rendering and COG terrain tooling** (`src/world/flannan/FarShore.js`, `tools/terrain/cog.mjs`).
   - The PR has no description and is closed unmerged, so it is low confidence. A far-shore silhouette layer and Cloud-Optimized-GeoTIFF terrain fetching are both directly relevant to the whole-world horizon branch.
   - Read the source before deciding. Effort: S to read. Targets, if adopted: `EarthTerrainStream.cpp`, `far/`.

Not recommended: PR #15's ResolutionGovernor (our `upscaler.rs` / DLSS path supersedes it) and all of its gameplay systems.

---

## 4. Two algorithms in detail

### 4a. Above-water to underwater camera transition (Tidewater `src/post/Underwater.js` @4811ba48)

**Lens model.** The camera's near clip plane *is* the lens; the water between the eye and the near plane is clipped away. The waterline on screen is therefore exactly where the rendered surface crosses the near plane. There is no global "underwater" switch: the medium is decided **per pixel** (`underwaterMedium(uv)`, rendered into a medium target).
1. `camH = camPos.y - waterHeightAtCamera` comes from GPU query slot 0.
2. Default: `water = camH < 0`.
3. If the water material wrote this pixel (`mask.y > 0.5`), `water = mask.x`. The mask records which side of the surface the fragment was seen from (front- or back-facing).
4. Otherwise, if `|camH| < STRADDLE` (0.35 m, `LENS_REACH`):
   - reconstruct the scene point from reversed-Z depth: `viewZ = n f / ((n - f) d - n)`;
   - a pixel with no depth (sky) is water if the ray points down (`dirW.y < 0`);
   - otherwise it is water if `q.y < waterHeightAtXZ(q.xz)`.
5. Farther than STRADDLE, the whole view is in the camera's medium.

**Waterline search.**
- Only when `|camH| < STRADDLE`.
- The composite samples the medium texture at ±8 and ±16 px vertically. If any differs from the centre, it walks i = 1..16 px up and down to the first flip: `lineDist = i - 0.5`, `lineDir = ±1`.
- This assumes no camera roll: the line runs roughly across the screen.

**Meniscus** (the rounded water edge acts as a cylindrical lens):
- `t = clamp(lineDist / band, 0, 1)` with `band = 9` px.
- `bendPx = t·sqrt(1 - t²)·band·0.8`. The scene is sampled at `uv - (0, lineDir·bendPx / H)`, pushed away from the line on both sides.
- Dark contact line: `line = smoothstep(2, 0, lineDist)`, multiplied by `(1 - 0.55·line)`.
- Bright rim: `rim = smoothstep(band, 0.4·band, lineDist)·smoothstep(0.5, 2.5, lineDist)`, added as `rim·0.15·(skyIrr·2.5 + sunColor·0.02)`.

**Fog switch (the medium itself), for pixels with medium = water.**
- Path length: `dist = min(|ray·viewZ|, 600) - near·|ray|` (the medium starts at the lens).
- Sun direction underwater: `Ls = -refract(-sunDir, +Y, 1/1.333)`, with `mu = max(Ls.y, 0.15)`.
- Phase: 0.75 × Henyey-Greenstein (g = 0.85) + 0.25 isotropic.
- Single scatter, integrated analytically along a ray whose depth changes linearly:

  ```
  _uwLit(m, sigT, zc, dirY, dist) = (exp(-a) - exp(-(a + k·dist))) / k
  a = sigT·zc/m,   k = sigT·(1 - dirY/m)
  ```

  `zc` is the camera depth. This form never overflows looking up through deep water, so no NaN reaches the TAA history.
- Multiple-scatter albedo term: `bb = 0.035·sigS`, `msAlb = 1.3·bb/(sigA + bb)`.
- `inSun = sunE·(sigS·phase + msAlb·sigT/π)·_uwLit(mu, …)`.
- `inAmb = π·skyIrr·0.9·(sigS/4π + msAlb·sigT/π)·_uwLit(0.8, …)`.
- `result = scene·exp(-sigT·dist) + inSun + inAmb + shafts`.
- **Shafts:**
  - A half-resolution pass of 20 steps over at most 22 m, with interleaved gradient noise jittered per frame and resolved by TAA.
  - Each step accumulates `(caustic(p, depth) - 1)·exp(-sigT·(s + z/mu))`.
  - Upsampled with depth awareness: bilinear weight / (10·|Δdist|/dist + 1)².
  - The torch is the same march: 8 steps through the flashlight cone with a Schlick-HG phase, ×3 strength.

**Our implementation.** `water_tw/tw_underwater.wgsl` + `underwater.rs` is a deliberate port of the above, with documented changes:
- The medium is decided in-pass (OP's water pass has one colour target, so there is no mask).
- STRADDLE is **1.0 m**.
- The water height near the camera comes from the **probe grid of the drawn mesh** (`probe.rs`, `tw_probe.wgsl`). Since W12d it takes the plane of the CDLOD triangle under the lens point, so the split is mesh-exact.
- Shafts run at full resolution with fewer steps.
- The torch is OP's own spot light within 2 m of the eye (`TORCH_REACH`).
- The meniscus band is 9 px (`BAND_PX`).
- Current-OP mode has its own froxel compositor (`underwater.rs` / `underwater_froxel.wgsl` at the `src/` root), and the C++ engage gate is in `EngineWgpu.cpp`.

**What is still worth adding:**
1. **Roll-aware waterline search.** Search along the screen projection of world-up instead of screen-vertical. OFP cameras roll in boats, aircraft and leaning infantry, and both TW and our port assume none. Re-implement. Effort: S. Target: `tw_underwater.wgsl` waterline loop (around l.230-245).
2. **Lens droplets on surfacing** (`LensDroplets.js`):
   - A grid of hashed cells per layer (pcg2d, 18 cells × 2 hashes per pixel).
   - Small drops cling and evaporate; large drops start sliding after a random delay and leave a wet trail.
   - Each drop is a tiny inverted, blurred lens with a sky highlight and a dark edge.
   - Applied at output resolution after the temporal resolve, so drops stay glued to the lens.
   - Trigger: `camH` crossing from below to above.
   - Port code. Effort: S. Target: new pass in `lib.rs` post chain after `temporal.rs`.
3. **Marine snow** (`MarineSnow.js`):
   - Procedural positions hashed per instance inside a camera-wrapping box; no simulation.
   - Drift with a slow current plus the swell orbital motion from the longest FFT cascades, decaying with depth.
   - Drawn as tiny depth-writing discs in the opaque pass so the underwater fog treats each one at its own distance.
   - Only below the surface.
   - Port code. Effort: S. Target: new `water_tw/marine_snow.rs` + `.wgsl`.

### 4b. Boat buoyancy (Tidewater `src/player/BoatController.js` @4811ba48)

**Model.** An 8.2 m, 3.2 t lobster boat as a rigid body.
- State: position, quaternion, linear and angular velocity.
- Centre of mass in the boat frame (+Z forward, +X port).
- Principal inertia (pitch, yaw, roll) = (14600, 15700, 3500 × 1.24) kg·m².

**Sampling points.** `model.hullSamples` are **waterplane patches**. Each has a local position, a patch `area` (m²) and a bottom Y. They are generated from the hull lines (`world/boat/HullBuilder.js`).
- They are squeezed to an "effective waterplane": x × 0.79; z pulled toward the centre of buoyancy × 0.9. This tunes GM ≈ 0.6 m, roll period ≈ 3.3 s, pitch ≈ 1.8 s, heave ≈ 1.25 s.
- `pitchStiffness = ρ g Σ area·(z - cbz)²` is used for the trim moment.
- 7 lateral keel stations `(z, lateral area)`: (-3.4, .73) … (3.2, .14).

**Water heights.**
- Queried on the GPU each frame at the sample points **predicted ahead by the readback latency**: `p + velocity·latency`.
- Results arrive 1–3 frames late.
- On arrival, the water's own vertical velocity is computed per sample:

  ```
  w = (h - h_prev - ½(g + g_prev)·Δx) / Δt
  ```

  `g` is the surface slope from the returned normal. Subtracting slope × displacement stops a hull moving across a wave face from reading it as rising water.
- The velocity is clamped to ±3 m/s and low-passed by 0.5.
- Between readbacks, `h_eff = h + w·age` (age ≤ 0.12 s). The jump when a new result lands is blended out with `exp(-h/0.06)`.
- Non-finite samples keep the last good value.

**Forces, integrated at a fixed 120 Hz** (at most 12 substeps per frame, `dt` capped at 0.1 s):
1. **Gravity:** `F = (0, -m g, 0)`.
2. **Buoyancy + heave damping, per sample, in world up.**
   - `depth = h_eff - p.y`. Skip if `depth ≤ 0`.
   - `sub = min(depth, 1.6)`, `wetK = min(1, sub/0.25)·area`.
   - Vertical velocity relative to the water: `vy = (v + ω×r).y - 0.6·w_water`.
   - `Fy = ρ g area sub - (1800·vy + 900·vy|vy|)·wetK`, with ρ = 1025.
   - Applied at the sample point, so it produces torque about the COM.
   - Bookkeeping: `wetFraction = Σ area·min(1, depth/0.3) / Σ area`, `wetD = min(1, 1.6·wet)`, and `imm` (immersion relative to the design draft).
3. **Calm-water resistance:**

   ```
   R = (40|u| + 22u² + 3000·smoothstep(|u|, 2.8, 5.4) + 55u²·smoothstep(|u|, 7, 11))·wetD
   ```

   This is friction plus the wave-making hump past hull speed. It acts along -forward at (0, -0.2, z_com).
4. **Air drag:** `-3.3·|v|·v`.
5. **Keel lateral hydrodynamics, per station.**
   - `vk = v_side + ω_yaw·dz - ω_roll·(y_lat - y_com)`.
   - `Y = -½ρA(0.5·|u|·vk + 1.1·vk|vk|)·wetD`: hull lift plus cross-flow drag.
   - Applied at (0, 0.06, z_station). This gives directional stability, the turning circle, speed lost in turns and outward heel.
6. **Propeller.**
   - rpm spools toward the lever (τ = 0.9 s up, 1.3 s down).
   - Ahead: `T = 26 kN·n²·max(0, 1 - u/(16·max(n, 0.2)))`. Astern: ×0.45.
   - Multiplied by `propWet = clamp(propSub/0.3 + 0.5, 0, 1)`.
   - Prop walk astern: a side force of 0.08·|T|.
7. **Rudder in the propeller race.**
   - `Ur² = u|u| + 0.9·2T/(ρ·0.14)`.
   - Angle of attack: `δ = 0.6·steer + atan2(v_rudder, |Ur|)`, clamped to ±0.7.
   - `lift = ½ρ·0.12·|Ur²|·2.8·sin δ cos δ·propWet`, applied at (0, -0.15, z_rudder) as a side force.
   - Plus induced drag `0.8·|lift sin δ|`.
8. **Running trim and dynamic lift.**
   - `trim = (2.8·ss(u, 2.5, 4.8) - 0.8·ss(u, 4.8, 7.5) - 0.4·ss(u, 7.5, 10.5))°`.
   - Torque `-pitchStiffness·trim·imm` about the side axis.
   - Lift `0.15·m g·ss(u, 3.5, 9)·imm` along boat up.
9. **Angular damping in the boat frame:** `(-25000·ωx, -2000·ωy, -(4500 + 900|u|)·ωz)·(0.2 + wetD)`. Roll damping grows with speed (keel lift).
10. **Mooring springs** when docked; terrain grounding at 8 keel and bilge points (penalty `400000·pen - 30000·min(vy, 0)`, friction 6000); pier piles via capsule resolution of 7 outline points.
11. **Integration:** semi-implicit Euler with **added mass in the boat frame**.
    - Added mass: sway +0.7, heave +0.05, surge +0.6.
    - Added inertia: pitch +1.0, yaw +0.4, roll +0.2.
    - Both are scaled by `wetD`.
    - The orientation is integrated from ω as an axis-angle quaternion about the COM; the origin is re-derived from the COM.
    - A non-finite state resets the boat to its berth.

**Ours.** `Ship.cpp:612-889`, inside `Ship::Simulate`:
- Contact points are the `GroundCollision` points of the hull geometry.
- Each submerged point gets `F = (1.2e6/baseMass)·mass·under`. For small hulls this is multiplied by a **planing gain**: 9× at rest to 12× flat out (W11c; the legacy value was 20–120×).
- Torque arm × 0.2. Drag is per-axis polynomials in body velocity × "drag depth" (`BoatWaterResponse.hpp`). Angular damping is `torqueFriction += angMomentum·0.7·under`.
- A non-physical righting spring toward `upWanted` (`:600-603`, `:886-889`).
- The water comes from `QueryWaterSurfaceScaled`, a 6-sine CPU sea tuned to Tidewater's amplitude (`WaterSurfaceQuery.cpp`). It is not the drawn FFT.
- Result (W11 report): hulls sit high or bounce, and top speed is 10–15 % low once the gain was reduced.
- Box3D floats only generic probes (AABB-fraction buoyancy, `Box3DBackend.cpp:265-331`). Boats are not Box3D bodies.

**Contrast, point by point:**
1. Tidewater's lift is ρgV per patch with **no gain**. Ours multiplies by 9–12×, which is why the draft and the hump never come out right.
2. Tidewater damps heave **against the water's own vertical velocity**, so the boat rides the swell without being kicked. Ours damps against absolute velocity.
3. Tidewater has added mass, a resistance hump and keel lift. We have none of them.
4. Tidewater has no righting spring: stability emerges from waterplane geometry. Ours has one.
5. Tidewater runs fixed 120 Hz substeps. Ours runs at the simulation tick.

**Recommendation: port the force model (MIT, attribute), not the GPU readback.**
- Keep `QueryWaterSurfaceScaled` as the height source. WRL §3.6 requires headless, off-screen and server operation without GPU readback.
- Extend the query to return the surface normal and `dh/dt` (analytic from the six sines, so the slope-advection correction becomes exact and no latency compensation is needed).
- New `ShipHydro` behind a switch, default off until the owner verdict:
  - waterplane patches derived from the hull geometry LOD (project the bottom onto a 0.5 m grid);
  - per-class mass, inertia and thrust from the vehicle config;
  - keep `PlaningWaterDragDepth` only under the legacy flag.
- Measure against the W11 harness (PBR, patrol boat): top speed, turning circle, draft at rest.

Effort: L. Targets: `engine/Poseidon/World/Entities/Vehicles/Misc/Ship.cpp`, new `Misc/ShipHydro.{hpp,cpp}`, `World/Terrain/WaterSurfaceQuery.{hpp,cpp}`, `BoatWaterResponse.hpp`, plus the Box3D probe water (`Box3DBackend.cpp` `ApplyProbeWater`, which can reuse the same patch integrator for crates and wrecks; S).

---

## 5. How Tidewater makes trees and plants, for the whole-world renderer

Sources @4811ba48: `src/world/Vegetation.js`, `vegetation/{PlantGeometry, VegNodes, VegMaterials, InstanceLOD, Impostors, Scatter, GrassField, LeafTextures}.js`, `materials/LODFade.js`.

**Geometry: procedural, once, in a local frame.** The base is at the origin and +Y is up. Each vertex carries semantic attributes so one mesh fits every instance:
- `aMat.x` = part id (trunk, frond, leaf card, bark…).
- `aVeg = (u height fraction, s along frond, flutter weight, phase)`.
- `aLobe = (offset to lobe centre, lobe id)`.

Broadleaf trees:
- A **buttressed trunk** forks into curved limbs: tapered tubes along quadratic Béziers with rotation-minimising frames.
- Limbs end in up to 8 **lobes** (leaf clusters `[x, y, z, r]`).
- Each lobe is filled with crossing **leaf-cluster cards**, biased to the shell with real gaps.
- Card normals blend the lobe sphere and the whole crown (volumetric shading).
- `aMat.y` = exposure (inner dark, outer lit) works as baked AO.
- Cards are emitted outermost-first so early-Z rejects the inner ones.

Leaves and fronds are a grid along a midrib. The **outline, slits, holes and tears are cut in the fragment shader** from (y, x) coordinates, and the grid only spans the outline envelope to limit discards. Trunks are a tube with height fraction `u`: the real height `H` is applied in the shader, so one mesh fits every height.

**Variants without more meshes.** A `LOBE_TABLE` (3 tree variants × 8 lobes, 2 shrub variants × 8) holds per-lobe scales (0 = dropped). The vertex shader picks a row from the instance seed, `floor(fract(seed·7.77)·nVariants)`, and scales or drops lobes, giving irregular crowns from one mesh. The same table drives the impostor bake, so near and far crowns match.

**Instancing.**
- Per instance: `iPos = (base.xyz, scale)` and `iDat = (lean azimuth, lean, H, seed)`.
- Merged meshes carry several plant kinds. The integer part of the seed is the kind, and vertices of the other kinds collapse to the base, so it stays one draw call.
- About 11 draws in total for all vegetation.
- `InstanceLOD`:
  - The near level holds instances within `range + margin`, refilled on the CPU from a uniform grid only after the camera moves a few metres. At most 2 types are refilled per frame, most overdue first, and a teleport refills everything.
  - The far level is a static buffer, optionally sorted front-to-back into 6 m buckets for early-Z.
  - The **exact** near/far split is done per instance in the vertex shader (`vegLodScale`), so switching is frame-exact and independent of refill cadence. Instances outside their window collapse to a point.

**LOD.**
- Near geometry within 65 m (trees) and 45 m (shrubs). Beyond that, **hemi-octahedral impostors**:
  - 6×6 frames per variant at 128 px.
  - Atlas A: brightness, leaf/bark flag, colour random and coverage. Atlas B: normal and AO.
  - Baked once at start-up from the same variant meshes. Colour is applied at runtime from the species palette.
  - Runtime: one camera-facing quad per tree, fitted to the projected extent.
  - The fragment shader picks the 3 frames around the view direction (barycentric blend within 100 m, the nearest frame beyond) and **re-projects the view ray onto each frame's plane**, which is exact for any direction and for non-uniform scale.
- **Cross-fade:** a Bayer 4×4 screen-door shifted every frame (`LODFade.js`). The incoming level keeps pixels below `fade` and the outgoing level keeps pixels at or above the same threshold, so every pixel is covered exactly once and TAA smooths it.
- **Distance thinning:** from 140 to 320 m, up to 50 % of crowns are removed by seed, and the survivors grow by `1/sqrt(1 - f)`, so canopy coverage stays constant. Shrubs are dropped beyond 180 m.

**Wind** (vertex shader, `vegPlantDeform`):
- `w = max(windSpeed·0.1, 0.03)`.
- **Travelling gust field:** `g = smoothstep(.46, .6, fbm((xz - gustOffset)/140)·.62 + fbm(…/61)·.38)`. The offset is integrated on the CPU so a change in wind speed or direction never makes the pattern jump.
- **Trunk sway** as a fraction of H:
  - `sway = w²·0.014·(0.9g + 0.3) + sin(0.83t + φ)·w·0.0065·(g + 0.45)`, plus a perpendicular `sin(0.61t)·w·0.0028`;
  - applied as `off·u²` along the stem, with the stem axis re-oriented, so the trunk **bends** rather than shears;
  - the crown follows the stem top, tilted half as much.
- **Fronds and leaves:** a length-preserving rotation about the crown centre of
  - `bend = s²·4.5·(w²·0.16·(0.8g + 0.35) + sin(…)·w·0.075·(g + 0.3))`,
  - plus a bounce, plus a normal flutter `flut·(sin(fast)·(0.045w + 0.008) + storm term)`,
  - so leaves stream downwind in storms without stretching.
- **Leaf translucency:** `back·forward³` scaled by the shadowed light colour.

**Grass** (`GrassField.js`):
- Camera-following 8 m cells, each drawing one instance of a blue-noise patch.
- Height comes from the heightmap, and an RGBA density mask selects 4 grass types.
- Three levels share identical blades: near < 18 m (3 segments), mid < 46 m, far < 88 m (1 triangle).
- Tiered blades fade by narrowing, and the survivors widen to keep coverage constant.
- Our `grass/` module already does a comparable blade ring, so this is not a gap.

**Our Earth trees today** (`engine/WgpuRenderer/EarthTreeMesh.hpp`, `EarthFeatureStream.cpp:150-226, 362`):
- Each tree is **28 CPU-expanded triangles**: a 4-sided trunk plus a two-ring, 5-sided crown.
- Coloured from a 4-entry palette and appended to one batch.
- No instancing, no wind, no LOD, no alpha, one shape.
- Seeded geographically (`GeographicSeed`), so appearance is stable. Capped by `Features::MaxTrees` / `MaxTriangles` and 16000 forest samples.

**Recommendation: re-implement the technique, and port the WGSL helpers (MIT).**
1. Have `EarthFeatureStream` emit **instance records** instead of expanded vertices: `pos`, `H`, `yaw`, `lean`, `seed`, species/biome id from latitude, climate and the existing snow-line logic. Keep `GeographicSeed`. Draw through the existing GPU-driven cull and indirect path.
2. Generate 3–4 procedural species meshes once in Rust: broadleaf lobed crown, conifer tiers, palm, shrub. Use the lobe-table variant trick.
3. **Port** `Impostors.js`'s hemi-octahedral bake and fragment re-projection, `LODFade.js` (Bayer cross-fade) and `vegPlantDeform` + `vegGustAt` (wind) as WGSL.
4. Use distance thinning with survivor growth for the 1–10 km Earth view.

The impostor idea was already flagged in `TIDEWATER-STREAMING-REVIEW-20260926.md`; Earth mode is its natural first consumer because it has no authored OFP trees to stay faithful to. Effort: M for instancing and wind; L with the impostor bake.
Targets:
- `engine/WgpuRenderer/EarthTreeMesh.hpp` (becomes a species mesh builder);
- `EarthFeatureStream.{hpp,cpp}` (instance records);
- `EngineWgpuEarth.cpp`;
- new `rust/src/earth_trees/` (`mod.rs`, `impostor_bake.wgsl`, `veg_wind.wgsl`);
- reuse `gfx3d/cull.wgsl` and `shaders/tree_snow.wgsl`.

---

## 6. threejs-particle-fluids: what would help us

**What it is.**
- **Position-Based Fluids** (Macklin & Müller) in an **XPBD** loop: 4 substeps × 2 iterations, density constraint with compliance (default 1e-4), XSPH viscosity, vorticity confinement, cohesion and surface tension, adhesion, a boundary-volume term.
- A **Morton-hashed GPU neighbour grid** with sort and neighbour lists (`src/core/hashGrid/*`).
- **SDF colliders** baked from meshes.
- The same particle core also does soft bodies (shape matching), cloth and gas.
- **Liquid rendering** (`fluids/render/SurfaceField.ts`): particles are splatted into a sparse 3D field with fixed-point atomic cloud-in-cell weights (`FIXED = 65536`, 8³ bricks to skip empty space), blurred, and the surface is ray-marched with refraction, absorption, a meniscus on wetted colliders and a voxel budget.
- **Smoke** (`gas/GasSystem.ts`): massless tracer particles advected by the fluid's velocities (Macklin 2014 §7.2.1), with optional heat sources (Boussinesq buoyancy, exponential cooling). Rendered either as sprites or by splatting into a 3D density texture (default 32×56×32) and ray-marching (`GasVolumeRenderer`, depth-composited).
- Scale: 5k–50k particles in a box of a few metres.

**Fit for us:**
- **Water splashes:** PBF at sea scale is the wrong tool. A boat or shell splash spans tens of metres, and our spray, impact and breaker particles (`water_tw/spray.rs`, `impacts.rs`, `boat_spray.rs`) are ballistic and cost about 0.06 ms. Skip.
- **Smoke:**
  - The splat-tracers-into-grid-then-march pipeline is what our SMK-038 field already does (`smoke_volume.rs`: 128×64×128 camera-anchored, scatter injection, `MAX_BLOBS = 2048`).
  - What we lack is plume dynamics: our new plumes re-sample wind per particle (`World/Effects/SmokeVolume.hpp:28-31`) but have no **heat-buoyancy or vorticity** coupling.
  - Two ideas to take: tracer advection by a coarse velocity field with Boussinesq heat, and its fixed-point CIC atomic splat (exact, order-independent, so no flicker). Re-implement. Effort: M.
- **GPU neighbour grid:** a reusable primitive if we ever want particle-particle or particle-hull interaction (spray hitting hulls, debris piles). Port when needed. Effort: S–M.

**Recommendation:**
- **Skip** the fluid solver itself.
- **Re-implement** the heat-buoyant tracer advection and the fixed-point CIC injection in the smoke field.
- Targets: `engine/WgpuRenderer/rust/src/smoke_volume.{rs,wgsl}` (injection), `engine/Poseidon/World/Effects/SmokeVolume.cpp` (heat and buoyancy per plume).
- Effort: M.

---

## 7. threejs-fire-pro: technique and a GPU fire system for our wgpu renderer

**Licence:** MIT, open source (§1).

**Technique** (README plus the source tree `src/engine/*.ts`, `src/engine/shaders/*.wgsl`). The shaders are already WGSL, so they are close to directly portable to wgpu.

- **Sparse, unbounded world-space grid.**
  - Tiles of 4×4×4 bricks; `brickSize` is 16 cells in the presets. The tile directory is an open-addressed hash with 27-neighbour tables.
  - Bricks are allocated and freed **on the GPU** with indirect dispatch.
  - Tiles cover sources and content plus a margin and linger a few steps, so the CPU never waits on readback.
  - Voxel budget `maxVoxels` (8 M in the presets); density `cutoff` 0.05 releases cells.
- **Solver**, on a staggered MAC velocity grid at `velocityDivisor` × the voxel size:
  - RK2 semi-Lagrangian advection, with **MacCormack** correction for scalars;
  - buoyancy from heat; smoke weight; damping; **vorticity confinement**;
  - force fields: wind, curl-noise turbulence, vortex, radial;
  - solid sphere, box and closed-mesh colliders;
  - **pressure projection by geometric multigrid V-cycles** on the bricks: weighted-Jacobi smoothing, residual restriction, trilinear correction, down to 2³, then three tile levels. Cold-started every substep.
  - Fixed 1/60 s step, at most one per frame (it slows down rather than skipping).
- **Combustion:**
  - fields: flame lifetime, heat, smoke, fuel;
  - fuel ignites above `ignitionHeat` and burns at `burnRate` (first order);
  - flame converts to heat, smoke and expansion; it cools at a set rate.
- **Rendering:**
  - ray-marched flame and smoke (128 steps, quadratic reconstruction filter, optional half resolution);
  - **blackbody** flame colour from a 512-texel RGBA16F LUT (temperature ≈ 2600 K in the presets) plus soot glow;
  - self-shadowing smoke through a coarser light volume (`lightingDivisor` 4);
  - depth-composited against opaque geometry, but **not** with reversed-Z, and not sorted with other transparents;
  - one point light per emitter or burning explosion.
- **Burst emitters** for explosions (radius, charge, outward speed, noise variation).

**What we have now:**
- **Legacy smoke and fire:** every puff is a full `Vehicle` (`Cloudlet : Smoke`, `Graphics/Rendering/Effects/Smokes.hpp:85`).
  - Emitters: `SmokeSource`, `DustSource`, `WeaponCloudsSource`, `WeaponFireSource :794`.
  - Other classes: `Explosion :625`, `Crater`, `ObjectDestructed :815`.
  - Burning wrecks use `SmokeSourceVehicle` with `_fire` / `_darkFire` (`:571`).
  - Hot particles are emissive sprites (`:283`).
- **New smoke (off by default):** `World/Effects/SmokeVolume.*` (one entity per plume), `SmokeSystem.*` (A/B harness), `SmokeDensityGrid.hpp`.
- **Renderer:** `smoke_volume.rs/.wgsl` (SMK-038 froxel field), `soft_particles.rs` (SMK-037).
- **Fire:** `World/Scene/Fireplace.hpp` and the muzzle flash (`gfx3d/muzzle_flash_tests.rs`). **There is no volumetric fire at all**: flames are sprites.

**How to build a modern GPU fire in our renderer (port code, MIT):**
1. **Scope.**
   - Local dense or sparse grids only around *active* fires: burning wrecks, fireplaces, explosions, napalm/fuel.
   - Budget: N simultaneous grids (for example 4), 0.1–0.25 m voxels, distance-LOD'd. Beyond about 150 m, fall back to the legacy sprites.
   - Smoke leaving the grid hands off to the existing SMK-038 field and plumes, so a long smoke column stays cheap.
2. **Port the solver WGSL:** `fluid-*.wgsl`, `pressure.wgsl`, `bricks.wgsl`, `pool.wgsl`, `curl-noise.wgsl`, `force-fields.wgsl`, `flame-common.wgsl`, `fire-emission.wgsl` and `BlackbodyLut.ts` into a new `rust/src/fire/`, with wgpu bind-group layouts. Keep its fixed 60 Hz step on our simulation clock.
3. **Rendering:**
   - Adapt the march to **reversed-Z**, which upstream does not support.
   - Composite it in the same slot as `smoke_volume`, so fire and smoke depth-sort together, and before the spray pass.
   - Use the half-resolution path with our temporal upscaler and DLSS history for noise.
4. **Lighting:** feed one local point light per active grid into our existing local-light list, which casts flicker onto terrain and objects. The flame colour comes from the blackbody LUT.
5. **Producers (C++):**
   - `Explosion` / `ObjectDestructed` → burst emitters, with `indirectHit` scaling the charge as the W8b water bursts do;
   - burning-wreck `SmokeSourceVehicle` → continuous emitter on the wreck's bounding box, with the wreck's Geometry LOD convex pieces (already handed to Box3D) as colliders;
   - `Fireplace` → small continuous emitter;
   - wind from the weather → the Wind force.
6. **Gate:** off by default, with an A/B mode in `SmokeSystem` like SMK-038. Measure GPU ms per grid (the README says it is "fill-rate and compute heavy").

Effort: L (solver port M, integration and producers M, perf/LOD M).
Targets:
- new `engine/WgpuRenderer/rust/src/fire/{mod.rs, *.wgsl}`;
- `rust/src/ffi.rs` + `include/wgpu_renderer.h` (fire emitter records; ABI bump);
- `lib.rs` (pass order next to `smoke_volume`);
- `engine/Poseidon/Graphics/Rendering/Effects/Smokes.cpp` (producers);
- `engine/Poseidon/World/Effects/SmokeSystem.*` (A/B);
- `engine/Poseidon/World/Scene/Fireplace.*`;
- `CREDITS.md` (Fire Pro MIT notice).

---

## Summary table

| # | Item | Action | Effort | Main targets |
| --- | --- | --- | --- | --- |
| 1 | Licences | All MIT: port with attribution; no assets; never consult threejs-water-pro | S | `CREDITS.md` |
| 2 | RefractionPass, hull cut-out, marine snow, lens droplets | Port code | M / M / S / S | `water_tw/` |
| 3 | PR #18 tiered paths; PR #15 refraction filter, particle velocity, camera cut; PR #10 pier contact | Re-implement | M / S / S / M | `water_tw/mod.rs`, `tw_water.wgsl`, `temporal.rs`, `Ship.cpp` |
| 4a | Roll-aware waterline search | Re-implement | S | `tw_underwater.wgsl` |
| 4b | Tidewater rigid-body boat on the CPU sea | Port the force model (not the readback) | L | `Ship.cpp`, new `ShipHydro.*`, `WaterSurfaceQuery.*` |
| 5 | Instanced procedural Earth trees + octahedral impostors + wind | Re-implement, port WGSL helpers | M–L | `EarthTreeMesh.hpp`, `EarthFeatureStream.cpp`, new `rust/src/earth_trees/` |
| 6 | Heat-buoyant tracer advection, CIC splat for smoke | Re-implement | M | `smoke_volume.*`, `SmokeVolume.cpp` |
| 7 | Volumetric GPU fire (sparse MAC grid, multigrid, blackbody march) | Port code | L | new `rust/src/fire/`, `Smokes.cpp`, `SmokeSystem.*` |
