# Nogova church placement investigation, 1 October 2026

The owner's reported freefly position is `(2592.94,5422.69,75.80)` in
`x,z,height` order, azimuth 100 degrees and elevation 6.2 degrees. A floating
church could not be identified in either installed capture at that pose.
This is a reproducibility limitation, not a finding that the owner's report
is false. No building placement, model origin or terrain was changed.

Root captured the installed matched EXE/DLL pair `7e2f1df7`, stamped
`codex/streaming-residency 7e2f1df7 2026-09-30_23:49 deploy-ps1`. The first
view at distance 2000 shows sandy hills. The repeat at requested distance 8000
has actual terrain, base and object ranges 7980, tactical visibility 8000,
rain zero and fog zero, and again has no identifiable church. The pair hashes
are unchanged after normal exit zero. Evidence is retained in
`build/weather-player/standard-us-clear-east-20260930-235443/owner-nogova-church.png`
and `build/weather-player/church-far-current-20261001-000219/`, including
`owner-nogova-church-far.png`, `result.json` and harness/log provenance.

The read-only stock source audit consumed the entire original `Noe.pbo`
member `noe.wrp`, an OPRW revision 3 world with 177224 placements, 345 model
names, 302 terrain textures and a 256-square float height grid. Its decoded
member SHA256 is
`9b27b11661d3cb30f9f0749a9a4616bfc58d723c4750666d1e674dd4213598d2`.
Twenty model paths contain the church name family `kostel`. None is within
approximately two kilometres of the reported camera. The two nearer eastward
candidates are:

| ID | Actual model | X, Z | Distance / azimuth | Final Y from installed probe |
| --- | --- | --- | --- | --- |
| 1328 | `data3d\kostel3.p3d` | 7362.711, 4772.176 | 4813.93 m / 97.77 degrees | 134.2333984 |
| 43298 | `data3d\kostel.p3d` | 6530.781, 4221.033 | 4117.11 m / 106.97 degrees | 117.1631241 |

The actual read-only `stream_identity_probe` confirms both IDs, exact models,
intact geometry, five normal vertex buffers, visual residency and complete
world frames. These final elevations agree with the stock WRP transforms
after `Landscape::ObjectCreateFromShape`'s existing legacy bounding-centre
correction, within ordinary float rounding. Classic OPRW3 uses this legacy
startup loader, not modern object streaming.

For ID 1328 the WRP Y is 134.2557983, model bounding-centre Y is 15.7834625 and
all visual LODs 0 through 4 have minimum Y -17.8453255. The existing correction
is approximately 0.02239 m; its resulting bottom is 116.3881 against centre
terrain 118.4498. All actual vertices within 10 cm of the bottom in these LODs
are 1.95 to 2.38 m **below** the cached terrain over the foundation footprint.
For ID 43298 the resulting bottom is approximately 107.9719 against terrain
109.9800; bottom vertices across every visual LOD are 1.99 to 2.03 m below
terrain. This audit does not identify either candidate as the reported church.

Both models' first visual LOD has ClipLand mask zero on every vertex. The
retained instance copies the actual `Object::Transform`, with rigid conform
mode zero. The subdivision/cache path preserves terrain-relative elevation
and notifies `SceneObjectMoved` after the final placement is restored.

The installed world log reports 2048-square terrain at 6.25 m. A read-only
audit of its existing L3 subdivision cache, SHA256
`16a5591a92a4aa11844502971b9bc1717a326789f86ee325030f26df575e85d7`, confirms
the same centre heights. A lightweight C++ witness using production
`TerrainCdlod.hpp` selection at the exact camera and default LOD controls
selects the level-3 patch at `(6400,3200)`, size 1600 m, morph band 4800 to
6400 m, over both candidates. Shader-equivalent vertex morph and triangle
interpolation give raster height approximately 118.4517 at ID 1328, only
2 mm above its full-resolution surface, and exactly 109.9800 at ID 43298.
Both permissible half-integer round conventions agree here. This is CPU
source evidence, not a measurement of the actual submitted terrain pixels.

The smallest useful next gate is identification of the visible church in an
actual owner capture, then its near foundation and far sightline with the
same installed pair. A shifted transform, missing foundation LOD, intervening
ridge or different camera can then be distinguished. The current evidence
does not justify sinking these candidates, changing all buildings, or
declaring a general terrain/placement fix accepted.
