param([Parameter(Mandatory)][string]$ToolsExe)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$expected=Join-Path $root 'content/default-packs/visual/material-overrides.json'
$deploy=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Deploy.ps1') -Raw
$guard=$deploy.IndexOf('& "$PSScriptRoot/Assert-DefaultMaterialPackage.ps1"')
foreach($mutation in @('Remove-Item -LiteralPath $stale.FullName','Copy-Item -LiteralPath $src')) {
    if($guard-lt 0 -or $deploy.IndexOf($mutation)-lt $guard){throw 'Deploy visual content guard moved after game-folder mutation'}
}
$work=Join-Path $root ('build/default-material-package-test/'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
function Pack([string]$name,[string]$text,[bool]$missing=$false,[bool]$current=$false) {
    $stage=Join-Path $work $name;New-Item -ItemType Directory -Path $stage -Force|Out-Null
    if($missing){Set-Content -LiteralPath (Join-Path $stage 'other.json') -Value '{}' -Encoding ASCII}
    elseif($current){Copy-Item -LiteralPath $expected -Destination (Join-Path $stage 'materials.json')}
    else{[IO.File]::WriteAllText((Join-Path $stage 'materials.json'),$text,[Text.UTF8Encoding]::new($false))}
    $archive=Join-Path $work ($name+'.pbo')
    & $ToolsExe pbo pack $stage $archive --prefix op_ground_materials | Out-Null
    if($LASTEXITCODE-ne 0){throw 'Private material fixture packing failed'}
    return $archive
}
function Check([string]$archive){return (& "$PSScriptRoot/Assert-DefaultMaterialPackage.ps1" -Archive $archive -ExpectedManifest $expected -ToolsExe $ToolsExe -EvidenceDirectory $work)}
$good=Pack 'current' '' $false $true
$proof=Check $good;if(!$proof.matches){throw 'Current copied source manifest did not pass'}
$source=Get-Content -LiteralPath $expected -Raw
foreach($kind in @('stale','albedo','archive','normal','detail','api')){
    $manifest=$source|ConvertFrom-Json
    $entry=@($manifest.materials|Where-Object{$_.source -ceq 'o\pt.paa'})[0]
    switch($kind){
        stale {$manifest.materials=@($manifest.materials|Where-Object{$_.source -cne 'o\pt.paa'})}
        albedo {$entry.albedo='op_ground_materials\sand_co.paa'}
        archive {$entry.sourceArchive='AddOns\unknown.pbo'}
        normal {$entry.normal='op_ground_materials\other_nohq.paa'}
        detail {$entry.detailNormalMetres=1}
        api {$manifest.api=2}
    }
    $archive=Pack $kind ($manifest|ConvertTo-Json -Depth 10)
    $rejected=$false;try{$null=Check $archive}catch{if($_.Exception.Message-match 'Stale visual material package|loader API'){$rejected=$true}else{throw}}
    if(!$rejected){throw "Material package falsifier accepted: $kind"}
}
foreach($kind in @('missing','oversize')){
    if($kind-eq 'missing'){$archive=Pack $kind '{}' $true}else{$archive=Pack $kind ((' '*65537)+$source)}
    $rejected=$false;try{$null=Check $archive}catch{if($_.Exception.Message-match 'has no materials.json|loader size limit'){$rejected=$true}else{throw}}
    if(!$rejected){throw "Material package falsifier accepted: $kind"}
}
Write-Host "Actual small-PBO material preflight tests PASS; private evidence $work; no game/installed content changed"
