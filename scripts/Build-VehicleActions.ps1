param([string]$Preset='win-x64-clang-rwdi', [string]$Python='python', [switch]$Preview)
$ErrorActionPreference='Stop'
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') {throw 'Unsupported content staging preset'}
$root=Split-Path $PSScriptRoot -Parent
$suffix=($Preset -split '-')[-1]
$stage=Join-Path $root 'build/jeep-ffv/animation-source'
$pack=if ($Preview) {Join-Path $root 'build/jeep-ffv/animation-preview'} else {Join-Path $root "dist/x64-win-$suffix/Mods/@OP_VehicleActions"}
$archive=if ($Preview) {'op_jeep_animation_preview.pbo'} else {'op_jeep_actions.pbo'}
$source=Join-Path $root 'content/default-packs/vehicle-actions'
$tools=Join-Path $root "dist/x64-win-$suffix/PoseidonTools.exe"
if (!(Test-Path -LiteralPath $tools)) {throw 'Build PoseidonTools before packaging VehicleActions'}
New-Item -ItemType Directory -Force $stage, (Join-Path $pack 'AddOns') | Out-Null
& $Python "$PSScriptRoot/default-content/generate_jeep_animations.py" --spec "$source/jeep-pose.json" --out $stage
if ($LASTEXITCODE -ne 0) {throw 'Jeep pose generation failed'}
& $Python "$PSScriptRoot/default-content/generate_jeep_animations.py" --spec "$source/jeep-pose.json" --override "$source/uh60-pose.json" --prefix uh60 --out $stage
if ($LASTEXITCODE -ne 0) {throw 'UH60 pose generation failed'}
Copy-Item -LiteralPath "$source/config.cpp" -Destination "$stage/config.cpp" -Force
$expected=@('config.cpp','jeep_idle.rtm','jeep_raise.rtm','jeep_aim.rtm','jeep_recoil.rtm','jeep_reload.rtm','jeep_lower.rtm')
$expected+=@('uh60_idle.rtm','uh60_raise.rtm','uh60_aim.rtm','uh60_recoil.rtm','uh60_reload.rtm','uh60_lower.rtm')
foreach ($file in Get-ChildItem -LiteralPath $stage -Force) {
    if ($file.PSIsContainer -or $file.Name -notin $expected) {throw "Unexpected animation staging file: $($file.FullName)"}
}
& $tools pbo pack $stage "$pack/AddOns/$archive" --prefix op_jeep_actions
if ($LASTEXITCODE -ne 0) {throw 'VehicleActions packaging failed'}
if (!$Preview) {Copy-Item -LiteralPath "$source/mod.json" -Destination "$pack/mod.json" -Force}
if (!$Preview) {
    New-Item -ItemType Directory -Force "$pack/Missions/OpenPoseidon" | Out-Null
    foreach ($mission in @('jeep-manual.Intro','jeep-ride.Intro')) {
        $old=[IO.Path]::GetFullPath("$pack/Missions/$mission")
        $new=[IO.Path]::GetFullPath("$pack/Missions/OpenPoseidon/$mission")
        if (!$old.StartsWith([IO.Path]::GetFullPath($pack)+'\') -or !$new.StartsWith([IO.Path]::GetFullPath($pack)+'\')) {throw 'Invalid staging path'}
        if (Test-Path -LiteralPath $old) {
            if (Test-Path -LiteralPath $new) {throw "Both old and grouped staging missions exist: $mission"}
            if ((Get-Item -LiteralPath $old).Attributes -band [IO.FileAttributes]::ReparsePoint) {throw 'Linked staging mission'}
            Move-Item -LiteralPath $old -Destination $new
        }
        Copy-Item -LiteralPath "$root/dev-missions/$mission" -Destination "$pack/Missions/OpenPoseidon" -Recurse -Force
    }
}
Write-Host "Staged own Jeep animations and one SP seat: $pack (preview=$Preview)"
