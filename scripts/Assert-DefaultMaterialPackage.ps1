param([Parameter(Mandatory)][string]$Archive,
      [Parameter(Mandatory)][string]$ExpectedManifest,
      [Parameter(Mandatory)][string]$ToolsExe,
      [Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
foreach ($path in @($Archive,$ExpectedManifest,$ToolsExe)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {throw "Material package preflight input missing: $path. Stage current content with scripts/Build-DefaultContent.ps1 before deployment."}
}
$out = Join-Path $EvidenceDirectory ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $out -Force | Out-Null
& $ToolsExe pbo extract $Archive $out --filter materials.json | Out-Null
if ($LASTEXITCODE -ne 0) {throw "Material package preflight could not extract materials.json: $Archive"}
$actual = Join-Path $out 'materials.json'
if (!(Test-Path -LiteralPath $actual -PathType Leaf)) {throw "Material package has no materials.json: $Archive. Stage current content with scripts/Build-DefaultContent.ps1."}
if ((Get-Item -LiteralPath $actual).Length -gt 65536 -or (Get-Item -LiteralPath $actual).Length -le 0) {throw 'Material package manifest violates the actual loader size limit'}
$catalog = Get-Content -LiteralPath $actual -Raw | ConvertFrom-Json
if ($catalog.api -ne 1 -or $null -eq $catalog.materials) {throw 'Material package manifest violates the actual loader API'}
$expectedSha = (Get-FileHash -LiteralPath $ExpectedManifest -Algorithm SHA256).Hash
$actualSha = (Get-FileHash -LiteralPath $actual -Algorithm SHA256).Hash
$record = [ordered]@{archive=(Resolve-Path -LiteralPath $Archive).Path;archiveSha256=(Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash;expectedManifest=(Resolve-Path -LiteralPath $ExpectedManifest).Path;expectedManifestSha256=$expectedSha;actualManifestSha256=$actualSha;extractedManifest=$actual;toolsSha256=(Get-FileHash -LiteralPath $ToolsExe -Algorithm SHA256).Hash;matches=($actualSha -ceq $expectedSha)}
$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'preflight.json') -Encoding UTF8
if (!$record.matches) {throw "Stale visual material package: $Archive has materials.json SHA256 $actualSha; current source requires $expectedSha. Run scripts/Build-DefaultContent.ps1 with the current preset/Python, then deploy. Evidence: $out"}
return [pscustomobject]$record
