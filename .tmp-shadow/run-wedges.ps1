$arms = [ordered]@{
  'base'     = @{}
  'prepass0' = @{ WGR_PREPASS = '0' }
  'occl0'    = @{ WGR_GPU_OCCLUSION = '0' }
  'nogpud'   = @{ WGR_GPU_DRIVEN = '0' }
}
& "$PSScriptRoot\capture-arms.ps1" -Out "$PSScriptRoot\wedges" `
  -World 'C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp' `
  -Addons 'C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons' `
  -Freefly '6907.68 6017.04 313.98 187.2 -1.7' -Hour 8 -ShotFrame 900 -Arms $arms
