param([string]$Preset='win-x64-clang-rwdi')
# Sinkhole W3 (SWIM-W3-001): stage the @OP_Swim default content pack -- content/default-packs/swim (config.cpp and
# the 23 Claude-made swim RTMs) packed as Mods/@OP_Swim/AddOns/swim.pbo (prefix "swim"), its mod.json, and the swim missions.
# Called by Build-DefaultContent.ps1; Deploy.ps1 installs the result.
$ErrorActionPreference='Stop'
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') {throw 'Unsupported content staging preset'}
$root=Split-Path $PSScriptRoot -Parent
$suffix=($Preset -split '-')[-1]
$source=Join-Path $root 'content/default-packs/swim'
$stage=Join-Path $root 'build/swim/pack-source'
$pack=Join-Path $root "dist/x64-win-$suffix/assets/Mods/@OP_Swim"
$tools=Join-Path $root "dist/x64-win-$suffix/PoseidonTools.exe"
if (!(Test-Path -LiteralPath $tools)) {throw 'Build PoseidonTools before packaging Swim'}
if (Test-Path -LiteralPath $stage) {Remove-Item -LiteralPath $stage -Recurse -Force}
New-Item -ItemType Directory -Force $stage, (Join-Path $pack 'AddOns') | Out-Null
$expected=@('swim','swimb','swiml','swimr','swimlb','swimrb','swimlf','swimrf','swimturnl','swimturnr','swimstart','swimstop','swimout','swimdead','swimdeadf','tread','fall','fallland','swimin','swimdive','swimunder','swimunderidle','swimsurface','swimfast') | ForEach-Object {"$_.rtm"}
foreach ($name in $expected) {
    $rtm=Join-Path $source "rtm/$name"
    if (!(Test-Path -LiteralPath $rtm)) {throw "Missing swim animation: $rtm"}
    Copy-Item -LiteralPath $rtm -Destination (Join-Path $stage $name)
}
Copy-Item -LiteralPath "$source/config.cpp" -Destination "$stage/config.cpp"
foreach ($file in Get-ChildItem -LiteralPath $stage -Force) {
    if ($file.PSIsContainer -or ($file.Name -notin $expected -and $file.Name -ne 'config.cpp')) {throw "Unexpected swim staging file: $($file.FullName)"}
}
& $tools pbo pack $stage "$pack/AddOns/swim.pbo" --prefix swim
if ($LASTEXITCODE -ne 0) {throw 'Swim packaging failed'}
Copy-Item -LiteralPath "$source/mod.json" -Destination "$pack/mod.json" -Force
New-Item -ItemType Directory -Force "$pack/Missions/OpenPoseidon" | Out-Null
foreach ($mission in @('swim-bay.Eden','swim-squad.Eden','swim-underfire.Eden')) {
    Copy-Item -LiteralPath "$root/dev-missions/$mission" -Destination "$pack/Missions/OpenPoseidon" -Recurse -Force
}
Write-Host "Staged swim moves: $pack"
