# OFP object satellite albedo, 12 September 2026

## Scope

Extend the existing automatic classic-world ground satmap with static roofs,
vegetation and other drawable map objects. Not an HLOD replacement, not a
native Reforger satellite implementation, and not a fix for every far-view bug.

`TerrainWgpu::BuildAutomaticSatmap` calls `BakeSatmapObjects` before creating
the existing mipmapped satellite texture. Temporary per-model 64x64 top views
use normal visual LOD geometry, source UVs and alpha-tested source textures.
Actual object transforms place the views in world coordinates. Small projected
footprints use area-weighted colour. Moving units are not scanned into the map.
The existing terrain shader transition and resource lifetime remain unchanged.

The normal default includes objects. `WGR_OFP_SATMAP_OBJECTS=0` restores the
ground-only bake; `WGR_OFP_SATMAP=0` disables the whole automatic satmap.
Restart/reload the world after changing these settings. No new disk cache:
mounted replacement texture sources are read again when the world is built.

## Evidence

Implementation 5ce3f450, diagnostic export 49238fe2. Installed test stamps
050c8ffb (documentation-only difference from 5ce3f450) and 49238fe2.
650 assertions in five satmap/crater/brush tests passed.

Fusion at [36154.60,23965.44,38053.39], az262.5/el-60.7, and at
[8260,11900,4000], az0/el-89: paired ground-only/object runs, native 1280x720,
MSAA4, DLSS off, isolated matching graphics profiles. All exited cleanly.
The 4 km screenshots still contain live 3D forest geometry and are not alone
proof of baked trees. Separate raw albedo exports provide that distinction:

- `build/fusion/acceptance-20260912-002154/satmap-albedo.png`: objects on.
- `build/fusion/acceptance-20260912-002338/satmap-albedo.png`: ground only.
- `build/everon-satellite-objects.png`: classic Everon, visible roads, roofs
  and repeated forest structures without the live 3D draw.
- Everon installed log/capture: `build/foliage-rays/acceptance/everon-satmap-49238fe2-rays1.*`.

Measured object-bake CPU time at world initialization:

| World | Models / textures | Stamped instances | Samples | Skipped | CPU ms |
|---|---|---:|---:|---:|---:|
| Fusion | 721 / 1289 | 405047 | 667638 | 2773 | 737.5, 624.0, 730.7 |
| Everon | 264 / 534 | 56367 | 122605 | 373 | 172.3 |

These are individual warm-machine observations, not load-time distributions
or FPS benchmarks. PNG export adds diagnostic I/O and is excluded from the
object-bake timings above. The initial Fusion total without export was 1535.3 ms.

GPU satellite storage is unchanged: about 21.3 MiB on Fusion, 5.3 MiB on
Everon including mips. Temporary caches cap models/textures separately at 2048;
height and pixel arrays scale with the already bounded satmap resolution.
Process peak RAM and VRAM were not measured. No frame-rate gain is claimed:
the existing live object geometry still renders, and this change adds no HLOD
or object eviction policy.

## Deliberate limitations

- Fusion is about 25 m/texel; individual houses occupy fractions of a texel.
  Classic Everon is about 12.5 m/texel. This is a far colour representation,
  not close-up roof detail or a replacement for silhouettes and 3D buildings.
- Original forest shapes/layouts remain regular. The bake preserves their
  visible pattern; it does not make OFP forests organically distributed.
- Empty, missing-texture and top-down-degenerate models can be skipped.
  Counts above are not a claim that every model or every crown is represented.
- Destroyed/moved static objects are not incrementally rebaked during a mission.
  Initial-state footprints can remain until world reload. Dynamic shadows,
  vehicles and units are not baked. Source assets may contain legacy shading.
- No new native Reforger path or resolution/visibility reduction is introduced.
- The owner's entirely blue Fusion islands at 38 km did not reproduce in the
  fresh profiles. That separate runtime/profile-dependent report remains open.

Optional inspection: `WGR_OFP_SATMAP_DUMP=<writable PNG path>` or
`Test-Fusion.ps1 -DumpSatmap` under the existing game lock. The export is off
normally and does not introduce frame-by-frame work.
