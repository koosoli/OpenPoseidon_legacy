# "Strange triangles inside many buildings" - owner says they are NOT the player model.
# My first capture put the camera 23 cm from where GameApplication grounds the player
# (SurfaceY+1.8 = 313.7508), so the frame was full of the soldier's own uniform at zero distance.
# Re-shoot the SAME interior at rising altitudes: the player stays grounded, so lifting the camera
# separates it from the body while (for the lower steps) staying inside the room.
# If the triangles survive at 315.5/317.0 they are NOT the player model.
$arms = [ordered]@{
  'alt313_98' = @{}   # the original, contaminated - kept as the reference
  'alt315_5'  = @{}
  'alt317_0'  = @{}
}
$alts = @{ 'alt313_98' = '313.98'; 'alt315_5' = '315.5'; 'alt317_0' = '317.0' }
foreach ($k in $arms.Keys) {
  & "$PSScriptRoot\capture-arms.ps1" `
    -Out "$PSScriptRoot\triangles2" `
    -World 'C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp' `
    -Addons 'C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons' `
    -Freefly "6907.68 6017.04 $($alts[$k]) 187.2 -1.7" -Hour 8 -ShotFrame 900 `
    -Arms ([ordered]@{ $k = $arms[$k] })
}
