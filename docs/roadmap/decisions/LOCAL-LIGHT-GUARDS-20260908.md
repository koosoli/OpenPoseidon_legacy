# Local-light visibility and camera-basis follow-up

## Implemented

- LGT-025 receiver lookup basis guard: compare the basis passed to LocalShadowVP
  against the published receiver camera, warn once for more than one metre or
  non-finite differences while local views exist. A different depth-pass origin
  is intentional and is not an error. No cache invalidation or matrix change.
- Attached WGPU bulb flare visibility uses view-geometry intersection excluding
  its own carrier, rather than relying on a coarse identity-free occlusion cell.
  Terrain visibility and the reflector's front-hemisphere/intensity gates remain.
  Non-attached lights, GL33 and the disabled bulb-glow path are unchanged.
- No larger marker, increased light intensity, shadow budget or new GPU pass.
- A second visibility blocker was found in EngineWgpu::DrawDecal: it ignored
  NoZBuf and tested screen flares at synthetic far-plane depth, also applying
  soft-particle fading. Both decal variants now honor NoZBuf. Ordinary smoke
  retains depth testing; the unrotated path retains its optional soft fade.
  Six policy assertions pin this separation; no shader or ABI change is needed.
- Carrier discovery also required fixing Light::AttachedOn: the base method was
  non-virtual and always returned null through the Light pointer in DrawFlares.
  All three attached-light classes now override it. A base-pointer dispatch test
  pins the contract and the reflector's visible-bulb switch.
- WGPU attached reflectors replace their grey HalfLight mesh with a compact
  screen-facing core, at the existing marker-size setting. Eight additive quads
  provide headroom without changing packed vertex/FFI layouts or other lights.
  The core bypasses diffuse eye accommodation, retains source visibility and
  front-facing checks, and is disabled for hidden-bulb head torches.

## Evidence and remaining work

- PoseidonGame/PoseidonTests build passed; ShadowMath: 681 assertions / 47 cases.
  Guard cases include the one-metre boundary, diagonal distance, inactive local
  shadows, non-finite input and a legitimate second-camera origin.
- Installed baseline is 700246eb. Private fixture .tmp-bulb/fixed.eden explicitly
  positions a crewed jeep and logs its coordinates; the original car-lit mission
  cannot be assumed to keep its initial vehicle position.
- Installed af66c260 versus 700246eb: positioned-before/after captures show the
  street-lamp flare restored by NoZBuf, with stable jeep position and 14 active
  local shadow views (cache reuse observed). No camera-basis warning or runtime
  validation error. That build's carrier exclusion remained inert until the
  virtual dispatch correction above; do not credit it with fixing self-occlusion.
- Installed 15e4df73 core-after capture removes the grey plate across the jeep.
  In the fixed lens ROI x=610..671, y=359..382, RGB-mean peak rises 122 -> 134
  while area mean falls 36.83 -> 20.22 against af66c260. Still too dim for a lit
  lens, so the core-only accumulation is raised from two to eight layers without
  increasing its footprint. The player is moved away from the freefly lens in
  this fixture revision to avoid camera-inside-player artifacts.
- Rear capture has foliage between the camera and jeep and no visible front
  bulb; that is not an unobstructed rear-hemisphere acceptance test.
- Eight-layer core runtime comparison is pending; appearance is not yet accepted.
- Indoor two-camera visual validation, additional shadow views and far impostor
  coverage remain open as listed in the original handover.

## Street-lamp occlusion follow-up

Owner reports lamp glow/cones through cars, hills and buildings. The NoZBuf
flare restoration exposed an incomplete source-visibility contract: accurate
view-geometry rays were only used for attached lights with glow enabled.
Unattached street lamps still relied on a coarse sphere shifted toward the
camera. WGPU terrain visibility also depended on the legacy enableHWTL flag,
and disabling the glow styling restored the backwards terrain ray.

WGPU now always traces terrain toward the source and tests view geometry for
every eligible local flare, excluding only an attached carrier and the
first-person camera carrier as before. Glow styling cannot disable visibility.
GL33 policy is unchanged. The existing intensity/behind-camera/terrain gates
bound the extra object queries; no additional GPU pass or shadow-map budget.
The optional glow diagnostic logs up to 32 source/blocker samples.

Private fixture `.tmp-bulb/occlusion.eden` holds an Ural between the camera and
an existing Everon street lamp. Baseline `ad817127`, `truck-before.png`, visibly
draws the lamp glow on the truck silhouette. A first fixture that attempted to
spawn the abstract `StreetLamp` type crashed the baseline and was discarded;
the working fixture uses an existing map lamp instead.

Build and existing light/decal/shadow-basis tests pass (359 assertions, 4 cases).
Fixed installed comparison pending. This changes source glare visibility, not
surface illumination/shadow allocation or volumetric-light transport; those
must not be claimed fixed by a source-ray change.
