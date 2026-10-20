param([string]$Preset='win-x64-clang-rwdi', [string]$Python='python')
# Stage the @OP_TidewaterWildlife default content pack: Tidewater's humpback whale (dgreenheck/tidewater, MIT) as the
# animal OP_TidewaterHumpback -- generate_tidewater_whale.py turns the committed export
# (content/default-packs/tidewater-wildlife/source, written by export_tidewater_whale.py) into the model, its skin
# and config.cpp, packed as Mods/@OP_TidewaterWildlife/AddOns/op_tidewater_wildlife.pbo (prefix
# "op_tidewater_wildlife"). Called by Build-DefaultContent.ps1; Deploy.ps1 installs the result.
$ErrorActionPreference='Stop'
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') {throw 'Unsupported content staging preset'}
$root=Split-Path $PSScriptRoot -Parent
$suffix=($Preset -split '-')[-1]
$source=Join-Path $root 'content/default-packs/tidewater-wildlife'
$generated=Join-Path $root 'build/tidewater-wildlife/generated'
$stage=Join-Path $root 'build/tidewater-wildlife/pack-source'
$pack=Join-Path $root "dist/x64-win-$suffix/assets/Mods/@OP_TidewaterWildlife"
$tools=Join-Path $root "dist/x64-win-$suffix/PoseidonTools.exe"
if (!(Test-Path -LiteralPath $tools)) {throw 'Build PoseidonTools before packaging the Tidewater wildlife'}
foreach ($dir in @($generated, $stage)) {if (Test-Path -LiteralPath $dir) {Remove-Item -LiteralPath $dir -Recurse -Force}}
New-Item -ItemType Directory -Force $generated, $stage, (Join-Path $pack 'AddOns') | Out-Null
& $Python "$PSScriptRoot/default-content/generate_tidewater_whale.py" --source "$source/source" --out $generated
if ($LASTEXITCODE -ne 0) {throw 'Tidewater wildlife generation failed'}
$expected=@('config.cpp')
Copy-Item -LiteralPath "$generated/config.cpp" -Destination "$stage/config.cpp"
# the whale and the pieces an explosion tears off it
foreach ($model in Get-ChildItem -LiteralPath $generated -Filter '*.p3d') {
    Copy-Item -LiteralPath $model.FullName -Destination "$stage/$($model.Name)"
    $expected+=$model.Name
}
# the generator lists its textures by PAA format
$textures=Get-Content -LiteralPath "$generated/textures.json" -Raw | ConvertFrom-Json
foreach ($format in @('DXT1','DXT5')) {
    foreach ($name in $textures.$format) {
        & $tools image convert "$generated/$name.png" "$stage/$name.paa" --format $format
        if ($LASTEXITCODE -ne 0) {throw "PAA encoding failed: $name"}
        $expected+="$name.paa"
    }
}
# Tidewater's sea sounds (CC0 field recordings; their credits go along)
New-Item -ItemType Directory -Force "$stage/sound" | Out-Null
foreach ($sound in Get-ChildItem -LiteralPath "$source/source/sound" -File) {
    if ($sound.Extension -notin @('.ogg', '.md')) {continue}
    Copy-Item -LiteralPath $sound.FullName -Destination "$stage/sound/$($sound.Name)"
}
$expected+='sound'
foreach ($file in Get-ChildItem -LiteralPath $stage -Force) {
    if (($file.PSIsContainer -and $file.Name -ne 'sound') -or $file.Name -notin $expected) {throw "Unexpected Tidewater wildlife staging file: $($file.FullName)"}
}
& $tools pbo pack $stage "$pack/AddOns/op_tidewater_wildlife.pbo" --prefix op_tidewater_wildlife
if ($LASTEXITCODE -ne 0) {throw 'Tidewater wildlife packaging failed'}
Copy-Item -LiteralPath "$source/mod.json" -Destination "$pack/mod.json" -Force
Write-Host "Staged the Tidewater wildlife: $pack"
