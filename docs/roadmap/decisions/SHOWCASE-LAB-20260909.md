# Showcase Lab - 2026-09-09

Owner requested a playable physics/lighting/material test map inspired by the
Esoterica first-person physics video. Delivered as an optional original addon
and a Single Mission on Desert, not a new WRP or a renderer replacement.

Final installed runtime: `15e8c7dd`, 2026-09-09 02:04, matched EXE/DLL.
The addon SHA256 is
`8E780872A8A5E309D487218B9FCF848F29352B4A1B5239EB3138E888263EA707`;
the installed copy and generated output match. Final asserted action smoke
exited 0; `[physics]` repeated with 1,529 assertions / 42 passing cases.

Implementation: `content/showcase/README.md`, `scripts/showcase/build_showcase.py`,
`scripts/Install-Showcase.ps1`, and mission-local `triPhysicsShowcase` actions.
42 initial bodies; selected throws/placement, reset/clear, warm/cool moving point
lights, labelled flat/normal/gloss gallery, day/night, three stock vehicles.
No stock config override, shader change, copied third-party art or new dependency.

## Evidence

- `8612410f`: first implementation; `cba5a43a`: corrected asset format/placement.
- `96c379c5`: zero-initialized SphereProbeDef velocity and regression test;
  corrected authored normal direction and labelled panels.
- `bbd550be`: oriented panel UVs; matched EXE/DLL and addon installed.
- Python generator tests: 2 passed; engine `[physics]`: 1,529 assertions / 42 cases.
- Installed menu test: `triAssertListText` returned OK; selection returned true;
  Play loaded the mission, `LabPanelNormal` existed and `SHOWCASE: ready` logged.
- Bound real `ribs_nohq.paa` on both normal/gloss materials; no upload failure.
- Screenshots inspected: `build/showcase/smoke/mission-menu.png`, `campus.png`,
  `materials-day.png`, `materials-night.png`. Labels and surface relief visible.
- Resting stack body 0 stayed at (9494.00, 30.14, 3504.00), clearance 0.303 m,
  across repeated traces rather than being launched sideways as in the first run.
- Script action reset, light mode, drop, night, clear exercised; unknown command
  returned REFUSED. Menu-run process exited 0.
- Additional action run: thrown sphere contacts displaced stack body 1 from
  (9494.86, 3504.00) to (9494.82, 3504.14); reset restored the field. Three further
  light throws returned OK, the fifth light returned REFUSED; cool-light capture
  showed the gallery lit blue. Clean process exit.

## Traps Found

1. `createVehicle` searches for a free position. Explicit `setPos` is needed for
   an authored layout, including half-height placement of centred models.
2. TGA metadata loads but this object GPU fallback expects a PAA payload. Use
   ARGB8888 PAA with mips, not an upload-failed flat panel masquerading as normal maps.
3. Model normals are stored inward; MeshBuild negates them. Outward-authored
   normals make the roof underside sunlit and the normal panels dark.
4. `Vector3`'s default constructor does not initialize velocity. SphereProbeDef
   now defaults velocity to zero; this also protects the existing brick-tower tool.
5. View/Fire Geometry sentinels are 6e15/7e15, not 4e13/5e13.

## Honest Scope

Playable experimental campus, not visual parity with the reference video. The
browser exposed the reference title/description; the video frames were not
inspected. Single-player diagnostic bodies are not saved or replicated. General
Physics-tab overrides can exceed the mission-action budget. No universal FPS
claim, vehicle-physics migration or closure of the older visual backlog.
The first lit-probe capture showed severe shading stripes on the diagnostic
primitives. The existing selfIllum setting only affected emitting lamps;
extending it to ordinary bodies selected additive rendering and made them
translucent. That workaround was rejected. ProbeMeshes' builders supplied
outward normals directly to Shape, although MeshBuild negates them. The direct
correction stores inward normals, matching the authored gallery and the engine
contract; ordinary bodies keep their original opaque lighting route.
Final `stack-impact.png` was inspected on `15e8c7dd`: opaque, correctly shaded
boxes and sphere, without the severe diagonal self-shading stripes of the
earlier run. The normal fix affects only procedural diagnostic meshes, not
retail models or global shader behavior.
