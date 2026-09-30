[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/forest-crown-components'
New-Item -ItemType Directory -Force -Path $output | Out-Null
# Compile the actual private renderer function, not a duplicate of its algorithm.
# Only the position/container types are stubbed; no renderer or GPU is initialized.
$source = Get-Content -Raw (Join-Path $repo 'engine/WgpuRenderer/EngineWgpu.cpp')
$pattern = '(?ms)^static uint32_t BuildForestCrownComponents\(.*?^\}'
$match = [regex]::Match($source, $pattern)
if (!$match.Success) { throw 'Actual forest component function not found.' }
$types = Get-Content -Raw (Join-Path $repo 'engine/Poseidon/Core/Types.hpp')
if ($types -notmatch 'typedef\s+int32_t\s+VertexIndex\s*;') {
    throw 'VertexIndex changed; update the standalone transport stub deliberately.'
}
$include = Join-Path $output 'forest_crown_components_under_test.inc'
[IO.File]::WriteAllText($include, $match.Value)
$exe = Join-Path $output 'forest_crown_components_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' '-I' $output (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_forest_crown_components.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Forest component regression compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Forest component regression assertions failed.' }
# Demonstrate that these fixtures catch the original defect, not just ordinary meshes.
$narrowed = $match.Value -replace 'uint32_t\(indices\[(t(?: \+ [12])?)\]\)', 'uint32_t(uint16_t(indices[$1]))'
if ($narrowed -eq $match.Value) { throw 'Index mutation did not find the production corner reads.' }
[IO.File]::WriteAllText($include, $narrowed)
try {
    $mutant = Join-Path $output 'forest_crown_components_narrowed.exe'
    & $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' '-I' $output (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_forest_crown_components.cpp') '-o' $mutant
    if ($LASTEXITCODE -ne 0) { throw 'Narrowed-index fixture failed to compile.' }
    & $mutant
    if ($LASTEXITCODE -eq 0) { throw 'Regression fixtures failed to catch 16-bit narrowing.' }
} finally {
    [IO.File]::WriteAllText($include, $match.Value)
}
Write-Host 'Actual forest component function: wide disjoint triangles, invalid corners, seam welds and empty mesh passed; original narrowing rejected.'
