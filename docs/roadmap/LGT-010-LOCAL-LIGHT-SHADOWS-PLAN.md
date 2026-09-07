# LGT-010 — shadows for local lights

**Status:** in progress, started 2026-09-07.
**Asked for by the owner:** *"wieso ist unsere sonne dynamisches licht was korrekt schatten
macht für gebäude, aber unsere objekte die self illuminating sind, so wie auch headlights
von fahrzeugen nicht"* and then *"moderne engines können doch auch mehrere lichter zum
schattenwurf bewegen, also sollten wir das doch auch können"*.

He is right. What follows is the plan, and it starts from a survey rather than an idea.

## The defect, measured

`lights_contrib` (`rust/src/shaders/lighting.wgsl:41`) computes distance attenuation, a cone
term for spots, and N·L. It contains **no occlusion term of any kind**. Every local light in
the game therefore shines through walls, through the vehicle carrying it, through everything.

Consequences already measured on 2026-09-07:

| what | measurement |
|---|---|
| Everon cafe interior, 23:00, street lamps ON | mean luma **17.95** |
| same frame, street lamps OFF | mean luma **0.00** |

The interior at night is not dimly lit. Every photon in it arrives from twenty street lamps
out in the road, straight through the walls; the engine has no indoor night ambient at all.
Vehicle headlights (`LightReflectorOnVehicle`, `VehicleAI.cpp:1164`) take the same
unshadowed path.

A camera-based gate was tried on 2026-09-07 and **rejected by the owner as a regression**
(`05438333`, reverted in `ab17f0e3`). It worked as designed and was still wrong: a gate that
follows the camera cannot let light through a window and pops at a doorway. See
[[no-half-measures-on-lighting]]. This document is the real fix instead.

## Why it is not "the same logic as the sun", and why it is still affordable

The sun is ONE light with ONE direction, so its shadow is an orthographic projection: four
cascade layers, four depth passes, and that is the whole shadow budget of the frame. A point
light shines in every direction and needs a cube — six passes. Twenty street lamps as point
lights would be 120 depth passes against the sun's four. That is the honest reason it was
never "just the same code".

What modern engines do about it, and what this plan does:

1. **An atlas, not a map per light.** One depth array; each shadowing light owns a layer.
   The renderer already has exactly this: `shadow_map` is a `texture_depth_2d_array` with a
   comparison sampler at `@group(0) @binding(1..2)`, and `ensure_shadow_target(device,
   resolution, count)` sizes it. A local light is one more layer.
2. **Spots first.** A cone needs ONE layer, like a second camera. Headlights are already
   spots (`LightReflector` → `LTSpotLight` → `WgrLight::dir.w`), and a street lamp points at
   the ground, so it can be one too. Points (six faces) are deliberately out of slice 1.
3. **Cache what cannot move.** A street lamp never moves and neither does the house beside
   it. Its layer is rendered once and re-used until something dynamic enters its volume.
   That is what makes twenty lamps affordable; headlights move and pay per frame, but there
   are few of them.
4. **Budget.** Only the best N lights get a layer. The rest behave exactly as today — no
   shadow, but no cliff either.

## What already exists, so it is not rebuilt

- `WgrShadowPass { count, resolution, light_vp[4], cam_pos }` and `WgrShadowCaster` with a
  **`cascade_mask` that is a u32** — four bits used, twenty-eight free. A shadow view is
  already addressed by a mask bit.
- Caster bucketing per view into instanced depth draws (`prepare_shadows`), the depth
  pipelines, and the pass-UBO ring, which already reserves slots past the cascades
  (`SKY_UBO_SLOT`) for the sky-visibility map — proof the ring takes more views than cascades.
- The GPU-driven set casts its own shadows (`draw_gpu_driven_shadow`).
- A second depth array sampled with the same comparison sampler already exists
  (`interior_sky_map`), so the shader pattern is established rather than novel.

`omni_count` in `WgrShadowPass` is NOT this: it selects a camera-distance cascade tier for
the SUN. Nothing in the tree does local-light shadows today.

## Slices

**Slice 1 — one spot casts a real shadow.** Extend the shadow target beyond the cascades,
add K local views, compute a perspective light view-projection per shadowing spot, mask
casters by the light's volume, sample it in `lights_contrib`. Acceptance: a vehicle at night
throws its own shadow and a wall's shadow, in a capture, with the frame cost measured
against the same pose with the feature off.

**Slice 2 — the budget and the selection.** N nearest/brightest, stable across frames (a
light that flickers in and out of the budget is worse than one that never had a shadow).

**Slice 3 — static caching.** Lamps render once; invalidate on a dynamic caster entering.
This is what makes a village affordable.

**Slice 4 — points.** Six faces or dual-paraboloid, only if slice 1-3 justify it.

## Rules this work holds itself to

- Ships OFF until measured, and the default only changes with the owner's word. The lesson
  of 05438333 is that a lever defaulted off is acceptable and a changed default is not.
- Every claim gets a capture the owner has seen, and a number from one binary.
- The ABI: add entry points, do not re-lay existing structs. `WgrLight` is size-asserted at
  64 bytes and `dir.w` already carries the spot flag; a shadow slot can ride there as
  `2 + slot` without touching the layout.

---

## What landed, 2026-09-07 (LGT-014 .. LGT-017)

Slice 1 and slice 4 both landed, and slice 4 arrived earlier than planned because
the owner rejected the alternative: LGT-012 tried to make lamps shadow-capable by
turning them into CONES, which changed what they LIT and made them vanish from
any angle outside the beam. The lesson from `05438333` repeated itself almost
exactly — do not change what a light does in order to make a feature possible.

- **LGT-014** undid that. It also found the older defect underneath it: the
  shader cut a local light at 10x its start attenuation, so a light's CORE SIZE
  and its RANGE were one number, and LAMP-004's small lamp core had quietly cut
  lamp reach from ~500 m to ~90 m. `LightDescription::endAttenScale` separates
  them. And `LightReflector::ToDraw` drew the bulb as well as the volumetric
  wedge, so suppressing the wedge made every vehicle look unlit.
- **LGT-015** gave point lights six-face cube shadows. What made six affordable
  was tiling, not memory: local views are now 4x4 tiles in ONE depth layer, so
  sixteen views cost what one used to. `dir.w` goes NEGATIVE for a shadowed
  point, because positive is what makes a light a spot.
- **LGT-016**: a light must not be occluded by the object it lives inside. The
  first cube capture was black because the probe's own emitting sphere filled all
  six faces. `LocalShadowSettings::selfRadius`.
- **LGT-017**: the beam ramp runs axis-to-rim now, not inner-angle-to-outer. The
  flat top inside the inner angle is what made a headlight read as a disc with an
  edge; the shaping exponent rides in `ambient.w`, the channel the inner angle
  vacated.

Verified in captures: street lamps and self-illuminating probes cast shadows and
stay omnidirectional; a night interior went from mean luma 22.03 to 1.53 with
light still arriving through the opening — real occlusion this time, not the
camera gate that was rejected; and an explosion's point light claims a six-face
cube for the ~0.2 s it lives (gauge: "0 of 1 lights are spots, 6 own a shadow
view"), which is the same path rockets and fire take.

**Slices 2 and 3 are still open**, and slice 3 is what a full village needs: the
budget is three lights / sixteen views, everything re-renders every frame, and a
street lamp's shadow never changes. Cache the static ones and invalidate on a
dynamic caster entering.
