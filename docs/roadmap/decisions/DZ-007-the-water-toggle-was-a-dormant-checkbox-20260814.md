# DZ-007 — The dev panel already had the water toggle; nothing was reading it

**Date:** 2026-08-14
**Status:** Wired and verified at runtime.

## The finding

The owner asked whether DayZ water's reflection could be turned off from the dev tools rather than an
environment variable. It turned out the control already existed and had never been connected:

* `Engine::MaterialDebugSettings::disableFresnelEnvironment` — `Graphics/Core/Engine.hpp:950`
* Dev-panel checkbox **"Disable Fresnel / Environment"** — `Dev/Debug/DebugOverlay.cpp:4033`, already
  bound to that field
* **Nothing consumed it.** A repo-wide grep returned those two lines and nothing else. It was a
  placeholder, reserved for exactly this feature and dormant until one existed.

Worth noting as a class of defect: a checkbox that does nothing is worse than no checkbox, because it
reads as a tested control. `disableSpecularGloss` beside it is in the same state today.

## What was done

`disableFresnelEnvironment` → **bit 3** of the material-debug flags word (`EngineWgpu.hpp`) →
`MATDBG_DISABLE_FRESNEL_ENV` in `frame.wgsl` → read **per fragment** from the frame UBO in both object
shaders. `SetMaterialDebugSettings` already calls `PushMaterialDebug`, so the checkbox takes effect
live with no restart and no model re-registration — the "switch, not baked state" design the existing
comment above `PushMaterialDebug` requires. `DebugOverlay.cpp` was not touched; the checkbox was
already bound.

The toggle suppresses **both** the reflection and the ripple, so one switch returns a water surface to
its pre-DZ-003 appearance.

## Measured at runtime (Enoch, toggle set vs clear, shipped binary)

| region | mean RGB, set → clear | Δσ | Δ high-frequency σ |
|---|---|---|---|
| pond | (1.90, 2.40, 3.56) → (23.78, 25.02, 30.37) | +3.408 | +0.288 |
| control, terrain | (22.06, 24.44, 29.90) → (22.07, 24.45, 29.92) | +0.001 | +0.001 |
| control, dark | (1.42, 1.28, 0.44) → (1.42, 1.28, 0.45) | +0.001 | −0.000 |

With the toggle set the pond returns to DayZ's authored near-black — both terms suppressed, not a
rippling black quad. Controls are identical to within 0.02/255. **0 uncaptured wgpu errors.**

## The environment variables are kept, and why

`WGR_ENV_REFLECTION` and `WGR_WATER_RIPPLE` still work. They are not redundant with the checkbox: a
before/after *capture* is two separate runs of the same binary, and it needs exactly one term to move
— the checkbox moves two. `WGR_MATERIAL_DISABLE_FRESNEL_ENV` was also added, writing the identical
`_materialDebug` field beside the existing `WGR_MATERIAL_DISABLE_NORMAL` /
`WGR_MATERIAL_INVERT_NORMAL_Y` overrides, so that the field-to-pixel path is demonstrated by evidence
rather than asserted.
