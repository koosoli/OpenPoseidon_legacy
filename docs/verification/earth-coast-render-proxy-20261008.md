# Earth coast render proxy

Source checkpoint based on `e5e6fc68`, isolated branch
`codex/earth-coast-depth-20261008`. The installed coast receipt
`build/earth-streaming/20261008-094943/result.json` passed its camera,
resident publication and FFT diagnostics, but `coast-1000m.png` failed visual
acceptance: wave crests appeared as white islands over dark gaps. This source
correction has CPU validation only. Combined build and the same installed
`Test-EarthStreaming.ps1 -Coast` run remain required; no new game was launched.

The opaque Earth near/far meshes previously retained Terrarium's near-zero
ocean surface heights. Those meshes hid wave troughs below their surface,
while water material depth treated ocean as shallow surf. Earth now uses a
render-only floor 200 m below the curved sea datum for corners whose uncurved
source elevation is in the inclusive interval **[-5, 0.5] m**. The upper
threshold tolerates near-zero elevation samples; the lower threshold avoids
raising genuine deeper negative terrain. This is a height heuristic, not a
coastline mask or measured bathymetry: low dry land in the interval can be
misclassified and visually submerged. Source land above 0.5 m and source
terrain below -5 m retain their original mesh height.

The water material applies the same corner rule before interpolating the
near mesh triangles `{a,c,b,b,c,d}`. Fragment depth uses that floor, avoiding
an interpolated water-vertex depth across mixed coastal triangles. The
existing 400 m off-map transition remains, relative to the curved sea datum.
The opaque overview sphere is also reduced radially by 200 m: its actual
equator/prime-meridian vertex at geographic origin (0,0) otherwise remains
exactly y=0 and hides a -4 m trough even when near/far ocean meshes are lowered.
The numeric test proves y=0 before and y=-200 after this radius adjustment.

`p.heights`, the uploaded source heightmap and the immutable published resident
ground are unchanged. This proxy supplies no gameplay seabed, bathymetry,
collision or swimming claim. Native OFP mesh generation and its water branch
are unchanged; shared water/kernel heightmap code is untouched.

Validation completed:

- `Test-EarthCoordinates.ps1` with clang++: curved sea floors at three local
  positions, trough clearance, preservation of positive/deeper negative
  source heights, both mixed coastal triangles and diagonal continuity,
  unchanged original resident ground, and the actual overview vertex case.
- `Test-EarthWaterShader.ps1` with matching existing host dependency libraries:
  the actual production composer validates the complete water material,
  wake, breaker and shore simulation modules without a GPU or game process.
- `git diff --check`.

Installed acceptance stays bounded to the existing Guernsey setup at
49.45,-2.55, altitude 1000 m, local camera [8000,1000,4800], azimuth 135,
elevation -20; await current ground/horizon publication and capture. Confirm
continuous visible sea with active FFT, then FFT retirement at 9000 m. The
saved pre-fix image is not evidence that coastal visuals passed.

Parent follow-up: matched `7a851e7a` installed at 10:11 and
`build/earth-streaming/20261008-101152/result.json` pass the same camera,
current ground/horizon, FFT-active and FFT-retired diagnostics. CPU coordinates
and all four complete production shader compositions pass in the integrated
tree. The inspected 1000m image has continuous water across most of the near
window instead of the repeated clipped trough islands. It still has a visible
near-water/overview transition, a dark corner artifact and a conspicuous
near/far shading mismatch. This is partial rendering progress, **not final
coastal visual acceptance** or a seamless worldwide ocean claim. The proxy
height heuristic and absence of measured bathymetry remain unchanged limits.
