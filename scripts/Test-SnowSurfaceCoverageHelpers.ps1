# Pure checks of the runner's actual production helpers; no process, game, TCP or GPU.
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'Test-SnowSurfaceCoverage.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$culture = [Globalization.CultureInfo]::InvariantCulture
foreach ($name in @('Require','Camera-Pose','Assert-SnowState','Decode-Eval','Assert-ObjectTelemetry','Set-FixedScene')) {
    $function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name },$true)
    if (!$function) { throw "Actual helper absent: $name" }
    . ([scriptblock]::Create($function.Extent.Text))
}
$script:checks = 0
function Check([bool]$condition,[string]$message) {
    if (!$condition) { throw $message }; ++$script:checks
}
function Refuses([scriptblock]$action) {
    $refused = $false
    try { & $action } catch { $refused = $true }
    Check $refused 'Invalid fixture input was accepted.'
}
$oldCulture = [Threading.Thread]::CurrentThread.CurrentCulture
try {
    [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('de-DE')
    Check ((Camera-Pose 9486 3004.5 205.75625 0 70) -ceq '9486 3004.5 205.75625 0 70') 'Camera formatting depends on host culture.'
    Refuses { Camera-Pose ([double]::NaN) 0 0 0 0 }
    Refuses { Camera-Pose 0 0 ([double]::PositiveInfinity) 0 0 }
} finally { [Threading.Thread]::CurrentThread.CurrentCulture = $oldCulture }
Check ((Decode-Eval '"data3d\mc vojakw2.p3d"') -ceq 'data3d\mc vojakw2.p3d') 'SQF literal model path was altered.'
Check ((Decode-Eval '"a""b"') -ceq 'a"b') 'SQF doubled quotes were not decoded.'
Check ((Decode-Eval 'false') -eq $false) 'Boolean evaluator value changed.'
$position = Decode-Eval '[9486,3006,207.549]'
Check ($position -is [array] -and $position.Count -eq 3 -and $position[2] -eq 207.549) 'Actual ASL array was flattened or altered.'
Refuses { Decode-Eval '"unfinished' }
Refuses { Decode-Eval '"bad"quote"' }
Refuses { Decode-Eval 'UNKNOWN COMMAND' }
$state = [pscustomobject]@{ enabled = $true; falling = $false; geometry = $true; depth = 0.18; chunks = 12 }
Assert-SnowState $state $true 0.18 12; ++$script:checks
Assert-SnowState $state $true 0.18 $null; ++$script:checks
Refuses { Assert-SnowState $state $false 0.18 12 }
Refuses { Assert-SnowState $state $true 0.19 12 }
Refuses { Assert-SnowState $state $true 0.18 11 }
$state.enabled = $false
Assert-SnowState $state $false 0.18 12; ++$script:checks
$state.falling = $true; Refuses { Assert-SnowState $state $false 0.18 12 }
Assert-SnowState $state $false 0.18 12 $true; ++$script:checks
$state.falling = $false
$state.geometry = $false; Refuses { Assert-SnowState $state $false 0.18 12 }; $state.geometry = $true
$state.depth = [double]::NaN; Refuses { Assert-SnowState $state $false 0.18 12 }; $state.depth = 0.18
$state.chunks = 1.5; Refuses { Assert-SnowState $state $false 0.18 $null }
Refuses { Assert-SnowState ([pscustomobject]@{enabled=$false}) $false 0.18 $null }
$line = 'wgpu object snow: enabled=1 deposit=0.1800 snowlineHeight=-1.00 snowlineRange=0.00 snowlineDepth=0.0000'
$actual = Assert-ObjectTelemetry ('[INFO] '+$line) $true 0.18
Check ($actual.deposit -eq 0.18 -and $actual.enabled) 'Actual uploaded deposit decoding changed.'
Refuses { Assert-ObjectTelemetry $line $false 0.18 }
Refuses { Assert-ObjectTelemetry $line $true 0.2 }
Refuses { Assert-ObjectTelemetry ($line.Replace('snowlineHeight=-1.00','snowlineHeight=0.00')) $true 0.18 }
Refuses { Assert-ObjectTelemetry ($line.Replace('snowlineDepth=0.0000','snowlineDepth=0.0600')) $true 0.18 }
Refuses { Assert-ObjectTelemetry 'unknown object snow' $true 0.18 }
$actual = Assert-ObjectTelemetry ($line.Replace('enabled=1 deposit=0.1800','enabled=0 deposit=0.0000')) $false 0.18
Check (!$actual.enabled -and $actual.deposit -eq 0) 'OFF arm must publish zero object deposit despite stored terrain cover.'
$script:replies = @{ 'triSetSimTime 100' = 'OK:100000'; 'triSetBrightness 1' = 'OK:1.000' }
function Eval([string]$code) { return $script:replies[$code] }
Set-FixedScene; ++$script:checks
$script:replies['triSetSimTime 100'] = 'OK'
Refuses { Set-FixedScene }
$script:replies['triSetSimTime 100'] = 'OK:100000'; $script:replies['triSetBrightness 1'] = 'OK:0.500'
Refuses { Set-FixedScene }
Write-Host "Snow surface actual helper checks passed: $checks. No game/GPU used."
