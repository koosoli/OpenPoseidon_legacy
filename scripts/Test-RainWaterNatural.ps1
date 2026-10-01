[CmdletBinding()]
param([string]$Compiler='clang++',[string]$OriginalSourceDirectory='',[string[]]$NativeCorpus=@())
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/rain-water-natural-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'rain_water_natural_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $root 'tests/unit/engine/weather/test_rain_water_natural.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) {throw 'Natural water production CPU compilation failed.'}
$inputs=@()
if ($OriginalSourceDirectory) {
    foreach ($world in @('eden','noe','abel','cain')) {
        $path=Join-Path $OriginalSourceDirectory ($world+'.rvw4.meta')
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) {throw "Missing exact original source metadata: $path"}
        $inputs+=,(Resolve-Path -LiteralPath $path).Path
        Write-Host ("Original 256-square/50m source {0} sha256={1}" -f $world,(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)
    }
}
foreach ($corpus in $NativeCorpus) {
    $data=Get-Content -LiteralPath $corpus -Raw | ConvertFrom-Json
    if ($data.side -ne 33 -or $data.order -ne 'z-major then x' -or $data.vertices.Count -ne 1089 -or
        !$data.header.readonly -or $data.header.baseline -cne $false -or !$data.header.ok -or
        $data.header.range -ne 2048 -or $data.header.spacing -ne 6.25 -or
        $data.before.time -ne $data.after.time -or $data.source.world -ne $data.map -or
        $data.source.wrpSha256 -notmatch '^[a-fA-F0-9]{64}$' -or !$data.installed.stamp -or
        $data.installed.files.Count -ne 2 -or
        ($data.before | ConvertTo-Json -Compress -Depth 10) -cne ($data.after | ConvertTo-Json -Compress -Depth 10)) {
        throw 'Native corpus lacks exact unedited, paused source provenance.'
    }
    $hash=(Get-FileHash -LiteralPath $corpus -Algorithm SHA256).Hash
    Write-Host ("Actual native corpus {0} sha256={1} wrp={2} stamp={3}" -f $data.map,$hash,$data.source.wrpSha256,$data.installed.stamp)
    $packed=Join-Path $output ('native-'+$hash+'.bin')
    $stream=[System.IO.File]::Create($packed)
    $writer=New-Object System.IO.BinaryWriter($stream)
    try {
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('RWNT'))
        $writer.Write([uint32]$data.header.range);$writer.Write([uint64]$data.header.heightRevision)
        $writer.Write([single]$data.header.spacing);$writer.Write([single]$data.origin[0])
        $writer.Write([single]$data.origin[1]);$writer.Write([single]$data.header.seaLevel)
        for ($i=0;$i -lt 1089;++$i) {
            $v=$data.vertices[$i]
            if ($v.Count -ne 3 -or $v[0] -ne $data.origin[0]+($i%33)*6.25 -or
                $v[1] -ne $data.origin[1]+[Math]::Floor($i/33)*6.25 -or
                [double]::IsNaN($v[2]) -or [double]::IsInfinity($v[2])) {throw 'Native corpus contains wrong coordinates or nonfinite height.'}
            $writer.Write([single]$v[2])
        }
    } finally {$writer.Dispose()}
    $inputs+=,$packed
}
& $exe @inputs
if ($LASTEXITCODE -ne 0) {throw 'Natural water CPU assertions failed.'}
if (!$inputs.Count) {Write-Host 'Original retail source replay not requested; actual native corpus and installed no-edit admission remain pending.'}
Write-Host 'Natural water CPU production checks passed; no renderer/build/game/visual claim.'
