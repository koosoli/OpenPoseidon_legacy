# WGR_ROAD_CPU_SPLIT=1 - predictions stated in advance so they can fail:
#   see-through goes (the road finally conforms), road stops clipping white by day,
#   road goes near-black at night like terrain, and ROAD_CPUPATH count drops to 0.
$W='C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp'
$A='C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons'
$cap="$PSScriptRoot\capture-arms.ps1"
# ridge pose, day
& $cap -Out "$PSScriptRoot\roadsplit\ridge" -World $W -Addons $A `
  -Freefly '7778.77 5661.41 521.81 106.7 -19.6' -Hour 8 -ShotFrame 900 -Arms ([ordered]@{
    'off' = @{ WGR_ROAD_TRACE='1' }
    'on'  = @{ WGR_ROAD_TRACE='1'; WGR_ROAD_CPU_SPLIT='1' } })
# night pose
& $cap -Out "$PSScriptRoot\roadsplit\night" -World $W -Addons $A `
  -Freefly '5852.26 6425.74 191.88 2.1 -10.3' -Hour 0 -ShotFrame 900 -Arms ([ordered]@{
    'off' = @{}
    'on'  = @{ WGR_ROAD_CPU_SPLIT='1' } })
