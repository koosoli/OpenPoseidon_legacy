# Handover — lighting, sky, view distance, flight (2026-09-08)

Written at the end of a long session so the next agent can pick up without re-deriving
anything. **Read this before touching the renderer.**

## Where everything is

Follow-up for the owner's Nogova report: see
[NOGOVA-VISUAL-20260908.md](decisions/NOGOVA-VISUAL-20260908.md).
It records the 50 km default/settings, float-depth coastline fix, zero-normal Davle
sign fix, blended-surface scattering and the held LLDR monitor. The branch/deploy
hashes below describe the earlier handover, not this follow-up.

Next follow-up: [ROTOR-SMOKE-WATER-20260908.md](decisions/ROTOR-SMOKE-WATER-20260908.md)
records rotor pose isolation, lamp controls, smoke measurements, boat drag and foam,
including the still-open visual checks and additional infantry/cloud reports.
The floating-body implementation and its limits are recorded in
[INFANTRY-WATER-20260908.md](decisions/INFANTRY-WATER-20260908.md).
Selective upstream review and the verified scene-preloader lifecycle port:
[UPSTREAM-REVIEW-20260908.md](decisions/UPSTREAM-REVIEW-20260908.md).
Water/cloud filtering, remaining body alignment and visual acceptance gaps, and
the owner's Waterline video reference:
[WATER-CLOUD-FILTERING-20260908.md](decisions/WATER-CLOUD-FILTERING-20260908.md).
Smoke wall/roof bounds, shared render-query caching and remaining indoor
performance/visual limits: [SMOKE-CONTACT-20260908.md](decisions/SMOKE-CONTACT-20260908.md).
Camera-basis diagnostic and attached-bulb visibility follow-up:
[LOCAL-LIGHT-GUARDS-20260908.md](decisions/LOCAL-LIGHT-GUARDS-20260908.md).

Experimental, mission-local terrain snow and its tested limits:
[EXPERIMENTAL-SNOW-20260909.md](decisions/EXPERIMENTAL-SNOW-20260909.md).
Playable optional physics/material campus and installed-menu evidence:
[SHOWCASE-LAB-20260909.md](decisions/SHOWCASE-LAB-20260909.md).
Rejected paired-density cloud experiment and the SDK history-mask caveat:
[CLOUD-GRAIN-EXPERIMENT-20260909.md](decisions/CLOUD-GRAIN-EXPERIMENT-20260909.md).

| | |
|---|---|
| Worktree | `.claude/worktrees/asset-obs` — **work here, not in the main checkout** |
| Branch | `asset-obs-001`, pushed to `safety/asset-obs-001` **and** `safety/consolidated-v2` |
| HEAD at handover | `b6cdbaa9` |
| Game folder | `D:\SteamLibrary\steamapps\common\ARMA Cold War Assault`, stamped `b6cdbaa9`, clean (no `+dirty`) |
| Working tree | clean — nothing uncommitted, nothing untracked outside `.tmp-*` |

The main checkout at the repo root is on `consolidated-v2` at an **old** commit. It looks like
work has been lost when you run `git log` there. It has not; you are in the wrong tree. Check
`git rev-parse --show-toplevel` before believing anything git tells you.

## Rules that cost this session real time

1. **`git commit -- <paths>` scopes FILES, not HUNKS.** In a shared worktree a file can hold
   two agents' work. `LGT-026` took `DebugOverlay.cpp` for two checkboxes and swept in another
   agent's half-finished block, leaving **HEAD unable to compile**. If you commit a file
   somebody else is also editing, read `git diff` of that file first, and afterwards prove
   HEAD builds on its own (`git stash push -u`, build, `git stash apply <sha>`).
2. **Run every game launch and deploy through `LOCK_OWNER="what you are doing"
   scripts/with-game-lock.sh <cmd>`.** The owner plays on this machine. The lock refuses if his
   instance is up — wait, never bypass.
3. **`--test-world-hour` also sets the DATE** to 1985-05-11, discarding the mission's. On that
   date the moon is below the horizon all night, so any moon/night-ambient capture using that
   flag measures a moonless sky and says nothing. Use a mission with the hour you want, and
   read the moon's real elevation from the `wgpu: moon:` log line. Force it with
   `WGR_MOON_AZEL=<az>,<el>` and `WGR_MOON_PHASE=<0..1>`.
4. **A capture pose can aim at nothing.** Freefly gives no feedback about what is in front of
   the camera; an empty direction is indistinguishable from a broken light. Render one frame
   with the feature OFF and look at it before measuring.
5. **A whole-frame on/off delta is mostly the sky.** Two runs are two launches and the sky is
   not identical between them; a window can dominate the difference. Difference the images and
   LOOK at the difference before quoting a number.
6. **Check which test binary owns the test.** `PoseidonCoreTests` and `PoseidonTests` are
   different executables; a green run of the wrong one has never seen your change.

## What landed, and the one-line reason for each

Lighting: `LGT-014` core size and reach are separate numbers; `LGT-015` point lights get
six-face cube shadows, 24 views tiled into one depth layer; `LGT-016` a light must not be
occluded by the object it lives inside; `LGT-017` beam ramps axis-to-rim, not
inner-angle-to-outer; `LGT-018/022` the lamp spill carries the far field, directionally;
`LGT-025` **each camera's shadow lookup must use its own origin** — this was "sometimes
everything goes black", and it needs two cameras (freefly over a player) to appear at all;
`LGT-026` static light shadows are cached, 13.38 → 8.17 ms frame; `LGT-027` light sprites blend
additive on wgpu as they always did on GL33.

Sky: `SKY-001` catalogue star field rotating about the celestial pole; `SKY-002` the real Yale
BSC5 and the NASA Milky Way, both compiled into the binary; `SKY-003` the cloud march was lit
BY the stars and handed them back; `SKY-004` the moon lights the cloud deck; `SKY-005` the band
is the photograph.

Other: `FACE-002` mouth movement via the existing bone palette; `VD-001` 10 km view distance;
`FAR-001` far plane separated from fog range; `FLY-001` aircraft ceiling; `GFX-001` Ultra is
the default preset; `NV-001` night vision is an image operation; `RFG-103` a load-time float
census for classic worlds.

## Open, in the order I would take them

1. **The vehicle headlight bulb still does not read as a lit lens.** Two blockers, both named
   and neither speculative. (a) `Occlusion::TestSphereWSpace` reports the headlight **fully
   occluded by the vehicle carrying it** — same shape as LGT-016 in a different system; the fix
   is to let the query ignore the light's own `AttachedOn()` object. (b) The marker sprite is a
   soft grey disc and 8-bit colour cannot exceed white, so additive alone does not make it
   intense. Do **not** try making it bigger — that has now been measured three times and
   produces fog (0.18 m: grey oval; 0.45 m: grey ball over the whole vehicle).
2. **Water speckles at altitude.** Horizontal scanlines and stipple above ~8 km. Pre-existing,
   **not** caused by FAR-001: reproduce with `POSEIDON_AERIAL_RANGE=0` and stock `--vd 10000`
   at 3,000 m. Lives in the water renderer.
3. **A guard for the LGT-025 class of bug.** Log when a camera's origin differs from the shadow
   basis by more than a metre while local shadows are active. Then "sometimes black" is a log
   line instead of a week. Belongs in `EngineWgpu.cpp` near the per-camera shadow block.
4. **The local-shadow view budget.** 24 views = 4 point lights, and the atlas already has 64
   tiles with 40 unused, so the memory is paid. But the ~1.25 ms residual is the per-pixel cost
   of SAMPLING the views in `lights_contrib` and no cache removes it. Raise only on its own
   measurement.
5. **Interior lighting from local light spheres.** The owner reported colourless, shadowless
   interiors. Not reproducible on the current build — most likely fixed by LGT-025, but that
   cannot be proved with a single-camera harness. A two-sided-normal theory was implemented,
   measured (interior got *darker*: 12.97 → 11.44 mean, lit area cut to a third) and
   **withdrawn**. Do not retry it without new evidence.
6. **Impostors for distant objects.** `rust/src/far/` is fully built and **completely inert on
   Everon, Malden and Kolgujev** — fed only from `_modernObjectPlacements`, which only exists on
   imported Enfusion worlds, and no log line says so. It becomes load-bearing the moment 10 km
   is pointed at a large imported world.
7. Parked, owner's call: two five-minute manual checks in `docs/roadmap/PARKED.md` (HRTF audio
   rows, mouse-wheel bindings).

## Things that are true and surprising

- **Shadow and particle quality tiers are ON/OFF only** at the engine (`GraphicsApply.cpp`).
  Low, Medium, High and Ultra are indistinguishable. The rows with teeth are terrain detail,
  object LOD and grass.
- **An empty vehicle has no headlights at all** — `TransportCore` only builds them when a
  `CommanderUnit` with AI is aboard. A parked unmanned truck is dark by design. Use
  `dev-missions/car-lit.eden` (driver `moveInDriver`'d) or `car-drive.eden` (on a waypoint).
- **Bloom cannot reach a night bulb.** The knee is scene-referred at 1.0 and nothing in a night
  frame exceeds it; at fifty times the shipped intensity the lamp head moves 14.6 → 14.8.
  The brightest pixel in a lit village is 241 on a **wall**; the bulb peaks at 199.
- The lamp gauge (`POSEIDON_LAMP_DIAG=1`) prints what a lamp's numbers actually resolve to.
  A street lamp is a 9 m bright core with **500 m** reach; the ~50 m edge players notice is
  the spill core, then inverse square — which is correct behaviour, not a cutoff.

## Dev switches added this session

`POSEIDON_LOCAL_SHADOWS`, `_LIGHTS`, `_SELF`, `_CACHE`, `_FORCE_REFRESH`, `_STATS`;
`POSEIDON_SPOT_BEAM_SHAPE`; `POSEIDON_LAMP_DIAG`, `_SPILL`, `_SPILL_RADIUS`, `_SPILL_AMBIENT`;
`POSEIDON_HEADLAMP`, `_RANGE`, `_BRIGHTNESS`, `_CONE`; `POSEIDON_BULB_GLOW`, `_STRENGTH`,
`_SIZE`, `_SIDE`, `_MARKER`, `_DIAG`; `POSEIDON_NV_FORCE`; `POSEIDON_LIGHT_GLARE`;
`POSEIDON_FLIGHT_MODERN`, `_PLANE_SCALE`, `_HELI_SCALE`; `POSEIDON_AERIAL_*`;
`WGR_STAR_OCCLUSION`, `WGR_MOON_CLOUDS`, `WGR_MOON_CLOUD_BLUE`, `WGR_MILKYWAY`,
`WGR_MILKYWAY_TEX`, `WGR_STAR_CATALOG`.

Dev-panel homes: Zeus tab → "Street lamps and light shadows" (10 sliders + shadow cache
checkboxes), Zeus tab → "Head torch" (strength and reach are now separate), Flight tab
(ceiling + aerial range), Sky → Stars.
