# Is the night self-illumination an authored EMISSIVE term?
#   trace       - WGR_ROAD_TRACE prints emi=(r,g,b) per road material. Non-zero closes it.
#   noemissive  - WGR_EMISSIVE_NIGHT=0 removes emissive at full night. Road goes black => confirmed.
# Terrain is the control: it is the only thing that already goes correctly black.
$arms = [ordered]@{
  'trace'      = @{ WGR_ROAD_TRACE = '1' }
  'noemissive' = @{ WGR_EMISSIVE_NIGHT = '0' }
}
& "$PSScriptRoot\capture-arms.ps1" `
  -Out "$PSScriptRoot\night-emissive" `
  -World 'C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp' `
  -Addons 'C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons' `
  -Freefly '5852.26 6425.74 191.88 2.1 -10.3' -Hour 0 -ShotFrame 900 -Arms $arms
