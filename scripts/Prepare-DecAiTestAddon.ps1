[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskTools = Join-Path $taskRoot 'dist/x64-win-rwdi/PoseidonTools.exe'
$taskSource = Join-Path $taskRoot 'tests/perf/addons/dec_ai_experimental'
$taskOutput = Join-Path $taskRoot 'build/dec-ai-test/@OP_DecAiExperimental/AddOns'
if (!(Test-Path -LiteralPath $taskTools)) { throw "Build PoseidonTools first: $taskTools" }
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskArchive = Join-Path $taskOutput 'op_dec_ai_experimental.pbo'
& $taskTools pbo pack $taskSource $taskArchive
if ($LASTEXITCODE) { throw 'Experimental addon packing failed.' }
Write-Host "Optional addon root: $taskOutput"
Write-Host 'Restart the installed game with --addon-root pointing to this directory.'
Write-Host 'Normal launches remain at the stock defaults; this is an opt-in test configuration.'
