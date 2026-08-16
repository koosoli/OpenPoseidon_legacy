# White roads: is it a texture bug or exposure clipping?
# The Takistan road capture on disk is 19.8% clipped at >=250 with terrain at ~(252,242,219).
# If the road becomes grey asphalt as exposure drops, "white roads" is tonemapping and every
# material-side investigation is at the wrong layer.
$arms = [ordered]@{
  'base'   = @{}
  'exp050' = @{ WGR_EXPOSURE = '0.50' }
  'exp025' = @{ WGR_EXPOSURE = '0.25' }
}
& "$PSScriptRoot\capture-arms.ps1" `
  -Out "$PSScriptRoot\white-roads" `
  -World 'C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp' `
  -Addons 'C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons' `
  -Freefly '6770.69 6099.78 312.86 230.7 -6.8' -Hour 12 -ShotFrame 900 -Arms $arms
