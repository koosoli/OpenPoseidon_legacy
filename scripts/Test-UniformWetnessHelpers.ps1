# Extract the real runner helpers. Pure checks: no game/process/network/GPU.
$ErrorActionPreference='Stop'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Test-UniformWetness.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$culture=[Globalization.CultureInfo]::InvariantCulture
foreach ($name in @('Require','Decode-Eval','Normalized-Wetness','Assert-Weather','Assert-Stationary','Camera-Pose','Assert-Paused','Parse-ClothTrace','Assert-WbCloth')) {
    $definition=$ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name },$true)
    if (!$definition) { throw "Actual helper missing: $name" }; . ([scriptblock]::Create($definition.Extent.Text))
}
$script:checks=0
function Check([bool]$condition) { if (!$condition) { throw 'Actual helper result differs.' }; ++$script:checks }
function Refuses([scriptblock]$action) { $refused=$false; try { & $action } catch { $refused=$true }; Check $refused }
Check ((Normalized-Wetness 0) -eq 0); Check ((Normalized-Wetness 1.0) -eq 1); Check ((Normalized-Wetness 0.359) -eq 0.359)
foreach ($invalid in @(-1,1.01,[double]::NaN,[double]::PositiveInfinity,$true,'0.3',@(0.3),$null)) { Refuses { Normalized-Wetness $invalid } }
Check ((Decode-Eval '"data3d\mc vojakw2.p3d"') -ceq 'data3d\mc vojakw2.p3d')
Check ((Decode-Eval '"a""b"') -ceq 'a"b'); Check ((Decode-Eval '0.359') -eq 0.359)
$position=Decode-Eval '[9475.25,3018.25,204.75]'
Check ($position -is [array] -and $position.Count -eq 3)
Refuses { Decode-Eval 'UNKNOWN COMMAND' }; Refuses { Decode-Eval '"broken"quote"' }
Assert-Weather ([pscustomobject]@{rain=0.85;fog=0}) 0.3 1; ++$script:checks
Assert-Weather ([pscustomobject]@{rain=0.01;fog=0}) 0 0.02; ++$script:checks
foreach ($weather in @(@{rain=0.2;fog=0},@{rain=1.1;fog=0},@{rain=0.8;fog=0.02},@{fog=0},@{rain=[double]::NaN;fog=0})) {
    Refuses { Assert-Weather $weather 0.3 1 }
}
Assert-Stationary $position $position 180 'civil'; ++$script:checks
Refuses { Assert-Stationary @($position[0]+0.1,$position[1],$position[2]) $position 180 'civil' }
Refuses { Assert-Stationary $position $position 0 'civil' }
Refuses { Assert-Stationary $position $position 180 'Stand' }
Refuses { Assert-Stationary @($position[0],[double]::NaN,$position[2]) $position 180 'civil' }
$oldCulture=[Threading.Thread]::CurrentThread.CurrentCulture
try {
    [Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('de-DE')
    Check ((Camera-Pose $position) -ceq '9475.25 3015.25 206.25 0 -6')
    Check ((Camera-Pose $position 1.5) -ceq '9475.25 3016.75 206.25 0 -6')
} finally { [Threading.Thread]::CurrentThread.CurrentCulture=$oldCulture }
Refuses { Camera-Pose @() }; Refuses { Camera-Pose @([double]::NaN,0,0) }
$first=@{time=32.5;weather=@{rain=0.82};actors=@{exposed=@{wetness=0.359};covered=@{wetness=0.0}}}
$second=@{time=32.5;weather=@{rain=0.82};actors=@{exposed=@{wetness=0.359};covered=@{wetness=0.0}}}
Assert-Paused $first $second; ++$script:checks
$second.time=32.502; Refuses { Assert-Paused $first $second }; $second.time=32.5
$second.weather.rain=0.81; Refuses { Assert-Paused $first $second }; $second.weather.rain=0.82
$second.actors.covered.wetness=0.001; Refuses { Assert-Paused $first $second }
$line='UNIFORM_CLOTH model=data3d\mc vojakw2.p3d texture=merged\00007mc_vojakw2.paa kind=2 enabled=true wet=0.3590 encoded=2.3590'
$row=Parse-ClothTrace ('[INFO] '+$line); $actual=Assert-WbCloth $row $true 0.35904
Check ($actual.kind -eq 2 -and $actual.encoded -eq 2.359)
Refuses { Assert-WbCloth $row $false 0.359 }
Refuses { Assert-WbCloth $row $true 0.48 }
$null=Assert-WbCloth $row $true 0.38; ++$script:checks
Refuses { Assert-WbCloth $row $true 0.32 }
$row.encoded=0; Refuses { Assert-WbCloth $row $true 0.359 }
$row.enabled=$false; $null=Assert-WbCloth $row $false 0.359; ++$script:checks
Refuses { Parse-ClothTrace 'unknown cloth trace' }
$sourceLine='[INFO] UNIFORM_CLOTH_SOURCE owner=0x123 model=data3d\mc vojakw2.p3d player=true primary=false wet=0.38 encoded=0'
Check (!($sourceLine -match '\bUNIFORM_CLOTH '))
Refuses { Parse-ClothTrace $sourceLine } # refusal trace is retained, never parsed as admission
Refuses { Parse-ClothTrace ($line.Replace('wet=0.3590','wet=-1.0000')) }
$row=Parse-ClothTrace ($line.Replace('kind=2','kind=1')); Refuses { Assert-WbCloth $row $true 0.359 }
$row=Parse-ClothTrace ($line.Replace('mc vojakw2.p3d','mc vojakw3.p3d')); Refuses { Assert-WbCloth $row $true 0.359 }
$row=Parse-ClothTrace ($line.Replace('00007mc_vojakw2.paa','head.paa')); Refuses { Assert-WbCloth $row $true 0.359 }
$row=Parse-ClothTrace ($line.Replace('\','/').Replace('enabled=true','enabled=1')); $null=Assert-WbCloth $row $true 0.359; ++$script:checks
Write-Host "Uniform actual helper checks passed: $checks. No game/GPU used."
