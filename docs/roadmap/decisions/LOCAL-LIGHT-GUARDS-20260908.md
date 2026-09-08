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
  screen-facing core, at the existing marker-size setting. Two additive quads
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
- Compact-core runtime comparison is pending; appearance is not yet accepted.
- Indoor two-camera visual validation, additional shadow views and far impostor
  coverage remain open as listed in the original handover.
