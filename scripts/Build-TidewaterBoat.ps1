param([string]$Preset='win-x64-clang-rwdi', [string]$Python='python')
# Stage the @OP_TidewaterBoat default content pack: Tidewater's procedural lobster boat (dgreenheck/tidewater, MIT)
# as the ship OP_TidewaterBoat -- generate_tidewater_boat.py turns the committed mesh export
# (content/default-packs/tidewater-boat/source, written by export_tidewater_boat.mjs) into the model, its
# textures (listed in textures.json) and config.cpp, packed as Mods/@OP_TidewaterBoat/AddOns/op_tidewater_boat.pbo (prefix
# "op_tidewater_boat"). Called by Build-DefaultContent.ps1; Deploy.ps1 installs the result.
$ErrorActionPreference='Stop'
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') {throw 'Unsupported content staging preset'}
$root=Split-Path $PSScriptRoot -Parent
$suffix=($Preset -split '-')[-1]
$source=Join-Path $root 'content/default-packs/tidewater-boat'
$generated=Join-Path $root 'build/tidewater-boat/generated'
$stage=Join-Path $root 'build/tidewater-boat/pack-source'
$pack=Join-Path $root "dist/x64-win-$suffix/Mods/@OP_TidewaterBoat"
$tools=Join-Path $root "dist/x64-win-$suffix/PoseidonTools.exe"
if (!(Test-Path -LiteralPath $tools)) {throw 'Build PoseidonTools before packaging the Tidewater boat'}
foreach ($dir in @($generated, $stage)) {if (Test-Path -LiteralPath $dir) {Remove-Item -LiteralPath $dir -Recurse -Force}}
New-Item -ItemType Directory -Force $generated, $stage, (Join-Path $pack 'AddOns') | Out-Null
& $Python "$PSScriptRoot/default-content/generate_tidewater_boat.py" --mesh "$source/source/tidewater_boat_mesh.bin.gz" --out $generated
if ($LASTEXITCODE -ne 0) {throw 'Tidewater boat generation failed'}
$expected=@('config.cpp','tidewater_boat.p3d')
Copy-Item -LiteralPath "$generated/config.cpp" -Destination "$stage/config.cpp"
Copy-Item -LiteralPath "$generated/tidewater_boat.p3d" -Destination "$stage/tidewater_boat.p3d"
# the generator lists its textures by PAA format (opaque DXT1; DXT5 for the glass and the traps' cut-out netting)
$textures=Get-Content -LiteralPath "$generated/textures.json" -Raw | ConvertFrom-Json
foreach ($format in @('DXT1','DXT5')) {
    foreach ($name in $textures.$format) {
        & $tools image convert "$generated/$name.png" "$stage/$name.paa" --format $format
        if ($LASTEXITCODE -ne 0) {throw "PAA encoding failed: $name"}
        $expected+="$name.paa"
    }
}
foreach ($file in Get-ChildItem -LiteralPath $stage -Force) {
    if ($file.PSIsContainer -or $file.Name -notin $expected) {throw "Unexpected Tidewater boat staging file: $($file.FullName)"}
}
& $tools pbo pack $stage "$pack/AddOns/op_tidewater_boat.pbo" --prefix op_tidewater_boat
if ($LASTEXITCODE -ne 0) {throw 'Tidewater boat packaging failed'}
Copy-Item -LiteralPath "$source/mod.json" -Destination "$pack/mod.json" -Force
Write-Host "Staged the Tidewater boat: $pack"
