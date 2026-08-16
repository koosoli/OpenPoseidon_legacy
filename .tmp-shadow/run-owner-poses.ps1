# The owner's three reported coordinates on Arma 2 Takistan.
# His in-game readout is already CLI order (X, Z, ALTITUDE) - verified against a known camera pose.
$W = 'C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp'
$A = 'C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons'
$cap = "$PSScriptRoot\capture-arms.ps1"

# 1. strange triangles + wobbling building walls, daylight
& $cap -Out "$PSScriptRoot\owner\triangles" -World $W -Addons $A `
  -Freefly '6907.68 6017.04 313.98 187.2 -1.7' -Hour 8 -ShotFrame 900 -Arms ([ordered]@{ 'day' = @{} })

# 2. roads visible through terrain, daylight
& $cap -Out "$PSScriptRoot\owner\road-seethrough" -World $W -Addons $A `
  -Freefly '7778.77 5661.41 521.81 106.7 -19.6' -Hour 8 -ShotFrame 900 -Arms ([ordered]@{ 'day' = @{} })

# 3. roads self-illuminating AT NIGHT. Two arms: night, and the same pose by day as the control -
#    "self-illuminating" is only meaningful against how the same surface reads when it is lit.
& $cap -Out "$PSScriptRoot\owner\road-night" -World $W -Addons $A `
  -Freefly '5852.26 6425.74 191.88 2.1 -10.3' -Hour 0 -ShotFrame 900 -Arms ([ordered]@{ 'night' = @{} })
& $cap -Out "$PSScriptRoot\owner\road-night" -World $W -Addons $A `
  -Freefly '5852.26 6425.74 191.88 2.1 -10.3' -Hour 12 -ShotFrame 900 -Arms ([ordered]@{ 'daycontrol' = @{} })
