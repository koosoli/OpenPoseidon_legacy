# Bug 2 confirmation: is the road poke-through the terrain-LOD chord mismatch?
#   base      - current default (decal bias already restored to ZRoadEpsilon parity)
#   lodpin    - WGR_TERRAIN_LOD_BASE=1000 forces leaves everywhere, morph_k 0. If the mechanism is
#               right the see-through should VANISH here, because road and terrain then agree.
#   lodcoarse - WGR_TERRAIN_LOD_BASE=0.25, the opposite arm; should get markedly WORSE.
#   raster    - WGR_CONFORM_RASTER=1, the actual fix: conform samples the rasterised lattice.
$arms = [ordered]@{
  'base'      = @{}
  'lodpin'    = @{ WGR_TERRAIN_LOD_BASE = '1000' }
  'lodcoarse' = @{ WGR_TERRAIN_LOD_BASE = '0.25' }
  'raster'    = @{ WGR_CONFORM_RASTER = '1' }
}
& "$PSScriptRoot\capture-arms.ps1" `
  -Out "$PSScriptRoot\conform" `
  -World 'C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp' `
  -Addons 'C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons' `
  -Freefly '7778.77 5661.41 521.81 106.7 -19.6' -Hour 8 -ShotFrame 900 -TimeoutMin 10 -Arms $arms
