param(
    [Parameter(Mandatory=$true)][string]$ControlInclude,
    [Parameter(Mandatory=$true)][string]$ControlLibrary,
    [Parameter(Mandatory=$true)][string]$PinInclude,
    [Parameter(Mandatory=$true)][string]$PinLibrary,
    [string]$Compiler = 'clang-cl',
    [string]$OutputDirectory = "$PSScriptRoot/../build/box3d-mass-comparison"
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path "$PSScriptRoot/../tests/experiments/box3d_mass_update.c").Path
$controlHeader = (Resolve-Path "$ControlInclude/box3d/box3d.h").Path
$pinHeader = (Resolve-Path "$PinInclude/box3d/box3d.h").Path
$ControlLibrary = (Resolve-Path $ControlLibrary).Path
$PinLibrary = (Resolve-Path $PinLibrary).Path
$controlHash = (Get-FileHash -Algorithm SHA256 $ControlLibrary).Hash
$pinHash = (Get-FileHash -Algorithm SHA256 $PinLibrary).Hash
if ($controlHash -ceq $pinHash) { throw 'Control and experimental libraries must be distinct.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = (Resolve-Path $OutputDirectory).Path
$result = [ordered]@{
    scope='Identical native library-only mass/inertia/impulse fixture; no game or dependency installation'
    sourceSha256=(Get-FileHash -Algorithm SHA256 $source).Hash
    controlLibrary=$ControlLibrary; controlSha256=$controlHash
    pinLibrary=$PinLibrary; pinSha256=$pinHash
    controlHeader=$controlHeader; controlHeaderSha256=(Get-FileHash -Algorithm SHA256 $controlHeader).Hash
    pinHeader=$pinHeader; pinHeaderSha256=(Get-FileHash -Algorithm SHA256 $pinHeader).Hash
    observations=@{}; passed=$false
}
try {
    foreach ($side in @('control','pin')) {
        $include = if ($side -eq 'control') {$ControlInclude} else {$PinInclude}
        $library = if ($side -eq 'control') {$ControlLibrary} else {$PinLibrary}
        $object = Join-Path $output "$side.obj"
        $exe = Join-Path $output "$side.exe"
        $compile = & $Compiler /nologo /c /std:c17 /MD /O2 "/I$include" $source "/Fo$object" 2>&1
        $code = $LASTEXITCODE
        $compile | Set-Content (Join-Path $output "$side-compile.log")
        if ($code -ne 0) { throw "$side compilation failed ($code)." }
        $link = & $Compiler /nologo $object $library "/Fe$exe" 2>&1
        $code = $LASTEXITCODE
        $link | Set-Content (Join-Path $output "$side-link.log")
        if ($code -ne 0) { throw "$side linking failed ($code)." }
        $lines = & $exe 2>&1
        $code = $LASTEXITCODE
        $text = $lines -join "`n"
        $lines | Set-Content (Join-Path $output "$side-result.log")
        $result.observations[$side] = @{exitCode=$code; actual=$text; exeSha256=(Get-FileHash -Algorithm SHA256 $exe).Hash}
        if ($side -eq 'pin') {
            if ($code -ne 0 -or $text -notmatch 'mass-update-regression PASS') { throw 'Experimental pin did not satisfy the actual mass/impulse oracle.' }
        } else {
            if ($code -ne 1 -or $text -notmatch 'immediateWorldInverseInertiaX[^\r\n]+FAIL' -or
                $text -notmatch 'immediateAngularVelocityX[^\r\n]+FAIL' -or
                $text -notmatch 'massAfterDensityEditAndStep[^\r\n]+PASS') {
                throw 'Released control did not reproduce the specific stale-inertia defect with the preservation control.'
            }
        }
    }
    $result.passed = $true
} finally {
    $result | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'result.json')
}
Write-Output "PASS: identical native fixture reproduces the released stale-inertia defect and validates the experimental pin. $output/result.json"
