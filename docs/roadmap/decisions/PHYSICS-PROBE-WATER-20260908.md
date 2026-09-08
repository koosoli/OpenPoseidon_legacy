# Physics-tab objects in water

## Implementation

- Physics-owned primitive/model probes now sample the existing CPU water query
  through an injected backend interface. Dry terrain rejects water; registered
  lakes/rivers use their own mean level and wave scale. Normal legacy vehicles
  and mission-owned characters do not use this new force path.
- Fresh-water buoyancy uses 1000 kg/m3 times submerged volume, versus the actual
  body mass. Shape volume is cached from Box3D mass properties and density at
  creation, including compound model shapes. Primitive mass is normalized to
  the requested kilograms rather than using a sphere volume for every shape.
- Spheres use spherical-cap immersion. Other hulls approximate partial immersion
  using world bounds and their full shape volume. This is not exact clipped-hull
  buoyancy or a multi-point righting-torque model. Overlapping compound hulls
  can overestimate displacement; open/hollow models do not simulate flooding.
- Implicit quadratic drag and angular damping slow submerged bodies. A rise
  bound stabilizes extremely low-density objects without teleporting them.
- Entry events depend on speed, size and coupled mass, including horizontal
  shore entry. Emit existing water ripples/foam plus 2..12 spray particles per
  impact. Surface motion gets weaker wakes at most every 0.15 s per body;
  entry cooldown and immersion hysteresis prevent repeated bobbing splashes.
  At most eight interaction callbacks run per physics tick.
- Physics-tab mass range is 0.01..10000 kg and explains density/displacement.
  Existing auto-probe harness gains POSEIDON_PHYSICS_PROBE_MASS. The auto-probe
  environment also enables a bounded entry/wake diagnostic.

## Verification

- PoseidonGame/PoseidonTests build passed.
- All physics tests pass: 1526 assertions / 41 cases. New tests cover floating
  versus sinking equal-radius bodies, dry land, 100 m/s entry of a 0.01 kg
  sphere, a 600 kg cubic-metre box settling to 60 percent immersion independent
  of the sphere-radius slider, and horizontal shore entry.
- First installed high-wave test completed normally and reported light bodies
  rising from depth and riding the CPU wave surface. It exposed two edge cases:
  ground-placement used the seabed and an initially submerged body could emit
  a fictitious surface impact. Placement now uses the local water/ground maximum;
  the first immersion observation cannot count as an impact. A regression test
  covers submerged initialization. Although the log said Shutdown complete,
  retaining the process handle established an actual post-shutdown access
  violation, not a successful exit. LLDB on the minidump traces it to the
  static s_solidObjects destructor freeing a mesh through an already destroyed
  WGPU uploader. Release probe pools before World/renderer teardown and replace
  raw static mesh caches with resettable owning references. Remount can rebuild
  the meshes rather than reusing freed pointers.
- Full-suite readback exposed uninitialized model halfExtents and ProbeSample
  axis. Initialize both; capsule axis follows its actual local X orientation.
  Existing world-isolation/worker determinism tests then pass unchanged.
- Installed final clean-exit Nogova light/heavy captures pending; private fixture
  `.tmp-bulb/probe-water.ps1` uses the actual Physics-tab auto-spawn route.
  Important profile trap: Nogova's key is `noe_noe_wrp.cfg`, not `noe.cfg`.
  The latter was ignored in earlier fixtures; verify Water look applied in logs.

## Limits

CPU waves approximate GPU FFT, not crest-for-crest alignment. This does not
resolve the previously reported corpse wave alignment. No new fluid solver or
GPU readback is introduced. Spray is the existing sprite emitter, not a fluid
sheet simulation. Projectile weapon effects remain on their existing path.
