# Foam: does the deep-edge pull-in and the large-scale patchiness read correctly?
# Stratis coastline, the same pose the previous foam work used (.tmp-bench/foam-new).
#   new     - current defaults: band_outer 1.10, patch on
#   nopatch - WGR_WATER_FOAM_PATCH=0, isolates the patchiness alone
#   legacy  - WGR_WATER_SHORE_FOAM_LEGACY=1, the pre-WTR look for reference
#   wide    - band_outer back to 1.55, isolates the deep-edge pull-in alone
$arms = [ordered]@{
  'new'     = @{}
  'nopatch' = @{ WGR_WATER_FOAM_PATCH = '0' }
  'wide'    = @{ WGR_WATER_FOAM_BAND_OUTER = '1.55' }
  'legacy'  = @{ WGR_WATER_SHORE_FOAM_LEGACY = '1' }
}
& "$PSScriptRoot\capture-arms.ps1" `
  -Out "$PSScriptRoot\foam" `
  -World 'C:\Users\mail\AppData\Local\Temp\ff-worlds\stratis\stratis.wrp' `
  -Mods 'a3stratis' `
  -Freefly '2200 5400 220 45 -8' -Hour 10 -ShotFrame 900 -Arms $arms
