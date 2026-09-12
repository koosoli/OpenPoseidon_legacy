param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Close the game before migrating content'}
$base=(Resolve-Path -LiteralPath $GameDir).Path.TrimEnd('\','/')
if (!(Test-Path -LiteralPath (Join-Path $base 'OpenPoseidon.exe'))) {throw 'Not an OpenPoseidon install'}
$plans=[Collections.Generic.List[object]]::new()
$directories=[Collections.Generic.List[string]]::new()
function CheckPath([string]$path) {
    $full=[IO.Path]::GetFullPath($path)
    if (!$full.StartsWith($base+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {throw "Outside install: $full"}
    for ($p=$full; $p -and $p -ne $base; $p=Split-Path $p -Parent) {
        if ((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw "Refusing linked content migration: $p"}
    }
    return $full
}
function PlanFile([string]$source,[string]$destination) {
    $destination=$destination -replace '(?i)([\\/]Missions[\\/])(FusionTour\.FusionOFP|ShowcaseLab\.Intro|SizeProbe\.FusionSizeProbe|jeep-manual\.Intro|jeep-ride\.Intro)([\\/])', '${1}OpenPoseidon\${2}${3}'
    $source=CheckPath $source; $destination=CheckPath $destination
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) {return}
    if ((Test-Path -LiteralPath $destination) -and
        ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $destination).Hash)) {throw "Different files at old/new paths; nothing moved: $source / $destination"}
    foreach ($prior in $plans) {
        if ($prior.Destination -eq $destination -and
            (Get-FileHash -LiteralPath $prior.Source).Hash -ne (Get-FileHash -LiteralPath $source).Hash) {throw "Conflicting sources; nothing moved: $source / $($prior.Source)"}
    }
    $plans.Add(@{Source=$source; Destination=$destination})
}
function PlanTree([string]$source,[string]$destination) {
    $source=CheckPath $source; $destination=CheckPath $destination
    if (!(Test-Path -LiteralPath $source -PathType Container)) {return}
    foreach ($entry in Get-ChildItem -LiteralPath $source -Recurse -Force) {
        $null=CheckPath $entry.FullName
        if ($entry.PSIsContainer) {$directories.Add($entry.FullName)}
        else {PlanFile $entry.FullName (Join-Path $destination $entry.FullName.Substring($source.Length+1))}
    }
    $directories.Add($source)
}
foreach ($name in @('@OP_VisualUpgrade','@OP_VehicleActions')) {
    $old=Join-Path $base $name
    if (Test-Path -LiteralPath $old) {
        $meta=Get-Content -LiteralPath (Join-Path $old 'mod.json') -Raw | ConvertFrom-Json
        if ($meta.defaultContentApi -cne '1') {throw "Unmanaged old package: $old"}
        PlanTree $old (Join-Path $base "Mods/$name")
    }
}
foreach ($pack in @(
    @{Name='@OP_Fusion'; Archive='fusion_ofp.pbo'; Mission='FusionTour.FusionOFP'},
    @{Name='@OP_Showcase'; Archive='showcase_lab.pbo'; Mission='ShowcaseLab.Intro'},
    @{Name='@OP_SizeProbe'; Archive='fusion_size_probe.pbo'; Mission='SizeProbe.FusionSizeProbe'})) {
    PlanFile (Join-Path $base "AddOns/$($pack.Archive)") (Join-Path $base "Mods/$($pack.Name)/AddOns/$($pack.Archive)")
    PlanTree (Join-Path $base "Missions/$($pack.Mission)") (Join-Path $base "Mods/$($pack.Name)/Missions/$($pack.Mission)")
}
foreach ($pack in @(
    @{Name='@OP_Fusion'; Missions=@('FusionTour.FusionOFP')},
    @{Name='@OP_Showcase'; Missions=@('ShowcaseLab.Intro')},
    @{Name='@OP_SizeProbe'; Missions=@('SizeProbe.FusionSizeProbe')},
    @{Name='@OP_VehicleActions'; Missions=@('jeep-manual.Intro','jeep-ride.Intro')})) {
    foreach ($mission in $pack.Missions) {
        PlanTree (Join-Path $base "Mods/$($pack.Name)/Missions/$mission") (Join-Path $base "Mods/$($pack.Name)/Missions/OpenPoseidon/$mission")
    }
}
foreach ($name in @('@OP_Fusion','@OP_Showcase','@OP_SizeProbe')) {
    $metadata=CheckPath (Join-Path $base "Mods/$name/mod.json")
    if (Test-Path -LiteralPath $metadata) {
        $existing=Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json
        if ($existing.defaultContentApi -cne '1' -or $existing.id -cne $name) {throw "Unmanaged target package: $metadata"}
    }
}
# All paths/conflicts are checked before the first move. Never recursively delete.
foreach ($item in $plans) {
    New-Item -ItemType Directory -Force (Split-Path $item.Destination -Parent) | Out-Null
    if (Test-Path -LiteralPath $item.Destination) {Remove-Item -LiteralPath $item.Source}
    else {Move-Item -LiteralPath $item.Source -Destination $item.Destination}
    Write-Host "Migrated: $($item.Source) -> $($item.Destination)"
}
foreach ($dir in ($directories | Select-Object -Unique | Sort-Object { $_.Length } -Descending)) {
    $null=CheckPath $dir
    if ((Test-Path -LiteralPath $dir) -and !(Get-ChildItem -LiteralPath $dir -Force | Select-Object -First 1)) {Remove-Item -LiteralPath $dir}
}
foreach ($name in @('@OP_Fusion','@OP_Showcase','@OP_SizeProbe')) {
    $folder=Join-Path $base "Mods/$name"
    if ((Test-Path -LiteralPath "$folder/AddOns") -and !(Test-Path -LiteralPath "$folder/mod.json")) {
        Copy-Item -LiteralPath "$PSScriptRoot/../content/default-packs/layout/$name.json" -Destination "$folder/mod.json" -Force
    }
}
