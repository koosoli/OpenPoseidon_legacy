# Live weather, terrain mist and later corpse hits

Owner reports at 09:06/09:13 prompted this finite correction after the first
fog/ragdoll handoff. Engine branch remains `codex/modernisation-followup-20261004`;
the immutable excavation backup and unrelated owner checkout were preserved.

## Weather and fog

Fog amount now edits world weather immediately, without Apply. Overcast does the
same and reads the live world value each frame, including changes made in Zeus.
The old private slider target no longer hides another control's update. The
expanded Overcast and forecast section retains the forecast freeze option.

The default volume follows actual native terrain. Layers can instead remain
at fixed sea-level heights, or partly follow hills. Broad concavity increases
valley density; coherent world-space noise makes thicker and thinner patches
without camera-relative jitter. Low ground fog, Two layers, Elevated band,
Valley mist and Mountain mist presets expose useful starting points. Advanced
controls expose terrain follow/reference altitude, valley boost/width, patch
strength/width/height and coverage-edge feather, alongside existing layer and
lighting controls. Shared config grows to ABI22 (80-byte fog/716-byte render
packet), with both C++/Rust layouts and advertised versions updated.

Covered terrain formerly suppressed the outdoor fog already integrated in
front of it, while covered tree sections admitted that same air. Terrain now
uses a dedicated receiver path; actual native/roof/cloud/overflow/camera gates
continue to exclude unsupported volume segments. This improves canopy-base
shadow consistency without changing the cast shadow or foliage alpha cards.

Object render range formerly clamped to the fog range and then raised back to
the larger objectsZ simulation radius. In heavy fog that admitted tree silhouettes
beyond supporting terrain. It now stays within the terrain/fog reach; the
simulation radius is unchanged. Clear-weather and aerial extensions remain.

Current native range test passed seven checks. Fog tests passed nine focused
actual GPU cases, seven production shader compositions and two atmosphere/source
checks. Installed source `108d2d84`, campaign
`owner-terrain-patch-moving-20261004-093701`, passed four normal-exit arms and
24 settled original Everon views at fog .6/zero, including native-qualified
airborne views and interpolated camera requests. Fogged terrain/object ranges
both measured 387 m; zero-fog ranges both measured 1480 m. Source, camera,
weather, pair-stability and encoding controls passed. Root inspected ground and
airborne ON/OFF images: foreground haze/ground shadows are more consistent,
terrain transitions are soft and no out-of-range unsupported tree row appears
in these views. The owner screenshots have no exact camera coordinates; this
is a repeatable comparison, not exact reproduction or universal streaming proof.

Published asynchronous transport samples were .275-.312 ms at 1280x720 for
ON/.6. They exclude background/weather coverage/consumer costs and are not an
overall or statistical performance guarantee. This is a terrain/noise density
model, not simulated air flow. Some authored foliage card shapes remain visible;
unknown/unready coverage is still refused. Broader maps/interiors/canopies remain
quality limits, not silently declared fixed.

## Corpse reactions and first-combat preparation

Settled automatic corpses released solver handles and returned before consuming
later shot/blast impulses. The new runtime recipe retains admitted hulls, mass,
joint frames and settled body transforms. A real pending hit can rebuild the
eleven bodies/ten joints at that pose within the existing four-active budget,
then transfer the impulse to the closest anatomical part inferred from the
force/torque line. There is no artificial impulse multiplier or standing-pose
reset. Stale world/terrain, invalid data and unsafe motion refuse wake safely.
Saved legacy frozen poses without the runtime recipe remain frozen after load.

The native terrain collider is now prepared when the feature is enabled or at
the first live mission simulation, before damage. The 16 MiB setup cost is
relocated, not eliminated or made asynchronous. The actual first installed
preparation took 238.8774 ms before fixture damage. Initial death motion also
no longer receives its inherited impulse a second time during initial readback.

Combined current-source native checks passed 77 cases / 14,476 assertions.
Game/Tests builds passed 233, 27 and 67-step staged increments. Matching engine
source `d465515a` deployed at 09:43 local. First installed hit attempt
`owner-frozen-hit-prepared-blast-20261004-094402` failed conservatively with
received/wake/transfer counters all zero. Diagnosis: createVehicle expects
height above terrain, but the runner supplied the measured ASL chest height,
placing the projectile above its target. The corrected runner sets and verifies
actual ASL position before releasing time; no injected damage or impulse is used.
The first corrected-runner retry stopped before time advanced because its
PowerShell loop variable used the reserved automatic `$error` name; this was
renamed. The positioned retry `owner-frozen-hit-positioned-final-20261004-095353`
then verified exact ASL position and 800 m/s native projectile velocity. Native
received hits = 2 and wake = 1 establish that the projectile reached the settled
corpse and rebuilt its bodies. Local transfers remained zero, so the compound
acceptance gate failed. This exposed an impulse handoff issue after wake; it
was not a missing collision or a reason to weaken the movement witness.
Final installed hit/blast result is recorded below after execution.


Final installed acceptance: matched engine source `921255c3`, stamped 10:06 local
(`+dirty` represented these documentation-only changes; all engine source was
committed/safety-pushed before build). Campaign
`owner-frozen-hit-same-wake-final-20261004-100656` passed normally with stable
installed EXE/DLL hashes and no forced termination:

- Four ordinary deaths used 44 bodies/40 joints; budget fallback, retained poses,
  runtime disable/enable and cleanup passed.
- A measured stock `BulletSingleW` collision at verified ASL position produced
  received=2, wakes=1, local transfers=1, anatomical part=0. Actual native local
  readback was 2.0431 m/s and 6.3913 rad/s. The whole consumed pose changed,
  resettled and returned to zero solver handles. Before the solver step, retained
  skin/equipment/root remained exact. There was no injected damage/impulse in
  this arm; it tests native ammo collision, not player mouse aiming.
- Actual frozen save/load restored four handle-free owners within 2 mm; active
  airborne save preserved the current pose/handles and refused manual freezing.
  Loading an active airborne save remains untested.
- The inherited 8 m/s horizontal / 10 m/s upward fixture passed the old 12 m
  cutoff while airborne, then landed and retired after 21.018 m total travel.
  The correctly positioned actual `GrenadeHand` killed a live stock target; its
  automatic rig returned to ground and retired after 14.470 m travel.
- Floor registration was 239.4965 ms / 16 MiB before damage. This relocation
  reduces first-combat setup work; it does not establish hitch-free rendering,
  eliminate per-corpse admission or provide asynchronous floor construction.

Final combined native suite: 77 cases / 14,487 assertions. Matching Game/Tests
build passed 67 steps. Root inspected the resettled projectile/grenade images;
corpses are retained on ground, but joint/skin appearance remains experimental.
Curated current/failed receipts are in
`weather-fog-corpse-feedback-20261004-runtime.json`; raw retail pose/hull arrays
and images remain ignored and local. Legacy frozen save recipes, crouch/prone,
water/excavation contact, multiplayer and broader model/LOD quality remain open.

This closes the finite owner-feedback implementation/checkpoint. Further visual
tuning is reserved for owner feedback; it does not restart the whole roadmap.
