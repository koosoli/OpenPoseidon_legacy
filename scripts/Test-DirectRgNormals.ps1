param([switch]$Legacy, [switch]$Decoded, [switch]$NormalViz, [switch]$Ag, [switch]$Nho)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($Ag -and $Nho) {throw 'Select AG or NHO, not both'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/direct-rg-normal'
$user=Join-Path $out 'user'
New-Item -ItemType Directory -Force $user | Out-Null
[IO.File]::WriteAllText((Join-Path $user 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
# Synthetic ENF1 BC7 mode 6: neutral RG normal, non-normal alpha=40/255.
# Original assets are referenced from the installed game, never redistributed.
$data=New-Object byte[] 172
function U32($at,[uint32]$v) { [BitConverter]::GetBytes($v).CopyTo($data,$at) }
[Text.Encoding]::ASCII.GetBytes('DDS ').CopyTo($data,0)
U32 4 124; U32 8 0x20000; U32 12 4; U32 16 4; U32 28 1
[Text.Encoding]::ASCII.GetBytes('ENF1').CopyTo($data,36)
U32 76 32; U32 80 4
[Text.Encoding]::ASCII.GetBytes('DX10').CopyTo($data,84)
U32 128 98; U32 132 3; U32 140 1; U32 148 0x59504f43; U32 152 16
$script:bit=0
function Bits([uint32]$v,[int]$count) {
    for ($i=0; $i -lt $count; ++$i) {
        $at=156+[int][Math]::Floor($script:bit/8)
        $data[$at]=$data[$at] -bor ((($v -shr $i) -band 1) -shl ($script:bit%8))
        ++$script:bit
    }
}
Bits 64 7
$channels=if ($Ag) {@(20,64,20,64)} else {@(64,64,20,20)}
foreach ($v in $channels) {Bits $v 7; Bits $v 7}
Bits 0 1; Bits 0 1; Bits 0 3
for ($i=1; $i -lt 16; ++$i) {Bits 0 4}
if ($script:bit -ne 128) {throw 'Malformed BC7 fixture'}
$normal=Join-Path $out $(if ($Ag) {'flat_nohq.edds'} elseif ($Nho) {'flat_nho.edds'} else {'flat_ntc.edds'})
[IO.File]::WriteAllBytes($normal,$data)
$arm=if ($Legacy) {'legacy'} else {'fixed'}
if ($Decoded) {$arm+='-decoded'}
if ($Ag) {$arm+='-ag'}
if ($Nho) {$arm+='-nho'}
if ($NormalViz) {$arm+='-normalviz'}
$settings=@{POSEIDON_USER_DIR=$user; WGR_GPU_DRIVEN='0'; WGR_AUTO_EXPOSURE='0'; WGR_EXPOSURE='0.15';
    WGR_MATERIAL_DEBUG_RVMAT='__unassigned__'; WGR_MATERIAL_DEBUG_VIEW='normal';
    WGR_MATERIAL_DEBUG_NORMAL=$normal; WGR_DIRECT_NORMAL_TRACE='1';
    WGR_LEGACY_DIRECT_NORMALS=($(if ($Legacy) {'1'} else {'0'}));
    POSEIDON_ENFUSION_COMPRESSED=($(if ($Decoded) {'0'} else {'1'}))}
if ($NormalViz) {$settings.WGR_MATERIAL_DEBUG_VIEW='normalviz'}
& "$PSScriptRoot/farfield-bench.ps1" -Label $arm -Mission (Join-Path $root 'dev-missions/snow-crawl.Intro') `
    -Out $out -Env $settings -Repeats 1 -Width 1280 -Height 720 -Windowed -WarmupSeconds 15 -TimeoutSeconds 90 -RequireAll `
    -ExtraArgs @('--test-world-freefly','9680','3590','33','0','-10',
        '--test-model','data3d/bedna_ammo.p3d','--test-model-distance','8','--test-model-scale','3')
$log=Join-Path (Join-Path $out $arm) 'run-01.log'
if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|failed to load' -Quiet) {throw 'Invalid direct normal run'}
$rgBound=Select-String -LiteralPath $log -SimpleMatch 'Direct RG normal bound:' -Quiet
if ($rgBound -eq [bool]$Ag) {throw 'Incorrect normal convention binding'}
if (!(Select-String -LiteralPath $log -SimpleMatch '--test-model: placed data3d/bedna_ammo.p3d' -Quiet)) {throw 'Crate fixture absent'}
if ($NormalViz) {
    Add-Type -AssemblyName System.Drawing
    $bitmap=[Drawing.Bitmap]::new((Join-Path (Join-Path $out $arm) 'run-01.png'))
    try {
        # Interior of the visible crate, not the sky or a diagnostic log counter.
        $pixel=$bitmap.GetPixel(640,530)
        $delta=[int]$pixel.G-[int]$pixel.R
        if ($Legacy -and !$Decoded -and !$Ag) {
            if ($delta -lt 10) {throw "Legacy AG tilt absent: $pixel"}
        } elseif ([Math]::Abs($delta) -gt 5 -or $pixel.B -lt $pixel.R+5) {
            throw "Neutral RG normal did not reach the visible crate: $pixel"
        }
        Write-Output "Visible normal fixture: $pixel"
    } finally {$bitmap.Dispose()}
}
