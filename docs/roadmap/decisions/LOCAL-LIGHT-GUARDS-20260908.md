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

## Evidence and remaining work

- PoseidonGame/PoseidonTests build passed; ShadowMath: 681 assertions / 47 cases.
  Guard cases include the one-metre boundary, diagonal distance, inactive local
  shadows, non-finite input and a legitimate second-camera origin.
- Installed baseline is 700246eb. Private fixture .tmp-bulb/fixed.eden explicitly
  positions a crewed jeep and logs its coordinates; the original car-lit mission
  cannot be assumed to keep its initial vehicle position.
- Runtime visibility comparison is pending. This is not acceptance of the bulb
  appearance: the old HalfLight marker is still grey and needs a separate fix.
- Indoor two-camera visual validation, additional shadow views and far impostor
  coverage remain open as listed in the original handover.
