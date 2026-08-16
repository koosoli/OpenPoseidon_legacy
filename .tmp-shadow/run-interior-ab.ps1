# THE decisive shadow A/B. The owner's "strange triangles" coordinate turned out to be INSIDE a
# building - the interior view I could not construct all night, and the exact case the cascade gate
# fix targets (a visible set that is entirely retained submits zero CPU casters).
$arms = [ordered]@{
  'fixed'  = @{ WGR_SHADOW_GATE_TRACE = '1' }
  'legacy' = @{ WGR_SHADOW_GATE_TRACE = '1'; WGR_SHADOW_LEGACY_GATE = '1' }
}
& "$PSScriptRoot\capture-arms.ps1" `
  -Out "$PSScriptRoot\interior-ab" `
  -World 'C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp' `
  -Addons 'C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons' `
  -Freefly '6907.68 6017.04 313.98 187.2 -1.7' -Hour 8 -ShotFrame 900 -Arms $arms
