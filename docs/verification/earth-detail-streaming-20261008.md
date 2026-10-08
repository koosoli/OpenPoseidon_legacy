# Earth preview: bounded progressive elevation

Source checkpoint based on `bdcedf02`, isolated branch
`codex/earth-detail-streaming-20261008`. Runtime installation and visual
acceptance are pending integration with the whole-world branch. No game was
launched or deployed for this source checkpoint.

## Behavior

- Earth preview alone changes its fixed 257 by 257 near heightfield from
  100 metre spacing / Terrarium zoom 10 / 25.6 km width to 25 metre spacing /
  zoom 12 / 6.4 km width on descent to 8 km altitude. Ascent returns to coarse
  detail at 12 km; between those heights the current band is retained.
- Recentring uses 3,200 metres in the coarse band and 800 metres in the fine
  band. Near heights, the matching far opening, the source-resolution blend
  and the edge skirts publish together. A newly requested cell/band clears
  any older ready result; an obsolete worker generation cannot publish.
- Decoded tiles retain the existing aggregate 64-tile limit. The existing
  aggregate 256-file disk limit now includes all supported zooms 3 through 12.
  Cache I/O and elevation downloads remain on the worker, outside rendering.
- Near ground and the horizon now share the Earth imagery mesh/material;
  the Earth preview no longer draws native CWA fixture terrain materials in
  its centre. Geographic UVs use the fixed Earth origin across recentres.
  Native heightmap uploads remain available to the existing renderer queries.
- Mesh normals use gradients of heights including spherical curvature.
  The combined near, horizon and skirts cost at most 256,000 triangles,
  independent of altitude. The separate overview globe remains 65,536
  triangles. Near and far boundary vertices use the same source coordinates;
  vertical skirts close the different edge tessellation between them.

This is progressive elevation in an opt-in visual preview. The NASA overview
image remains coarse; finer elevation does not provide detailed ground
imagery, worldwide objects, collision, simulation or an editable world.
Worldgen content and assets have not been imported. Ordinary OFP terrain
rendering is unchanged.

## Source checks

- `pwsh -NoProfile -File scripts/Test-EarthCoordinates.ps1`: PASS.
  Checks both resolutions, near/far boundary sample identity, skirts' bounds,
  recenter invariance, coordinate limits, dateline wrapping, monotonic horizon
  coverage and altitude-band hysteresis in addition to existing globe math.
- `EarthTerrainStream.cpp`: standalone Clang C++20 syntax check with
  `-Wall -Wextra -Werror`, production Windows macro definitions and the shared
  vcpkg headers: PASS.
- `TerrainWgpu.cpp`: Clang-CL `/Zs` syntax check with production compile-database
  flags, isolated source headers and shared generated/dependency headers: PASS.
- `git diff --check`: PASS.

## Integration acceptance still required

Build the combined branch and run installed Earth preview acceptance. Check
that the near centre uses the same imagery as the horizon, that descent below
8 km reports grid 25 / source zoom 12, and ascent above 12 km reports grid 100 /
source zoom 10. Move across both recenter grids and inspect the near/far edge
for cracks or imagery drift; change altitude inside 8--12 km to check that
the detail band does not churn. Verify ordinary OFP terrain remains normal.
