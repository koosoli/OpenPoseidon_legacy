<# Offline upgrade of an existing official 6.8 cache. Default prepares/reports only.
   -Apply installs verified depth files and backs up our previous add-on; user
   ReShade.ini, color preset, session files and graphics preferences stay intact. #>
[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$LocalSources,
 [string]$RuntimeDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/reshade/6.8.0'),
 [string]$StageDirectory='',
 [switch]$Apply
)
$ErrorActionPreference='Stop'
function RequireGameClosed {if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Close the game before preparing/upgrading the optional runtime.'}}
RequireGameClosed
$repo=Split-Path $PSScriptRoot -Parent
if(!$StageDirectory){$StageDirectory=Join-Path $repo ('build/reshade-scene-upgrade/'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))}
$RuntimeDirectory=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$LocalSources=(Resolve-Path -LiteralPath $LocalSources).Path
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory)
$helper=Join-Path $PSScriptRoot 'Upgrade-ReShadeSceneCache.py'
python $helper prepare $RuntimeDirectory $StageDirectory --local-sources $LocalSources
if($LASTEXITCODE-ne0){throw 'Offline stage preparation failed; runtime unchanged.'}
& (Join-Path $PSScriptRoot 'Build-ReShadeBridge.ps1') -RuntimeDirectory $StageDirectory -BuildDirectory (Join-Path $StageDirectory 'addon-build')
RequireGameClosed
$arguments=@($helper,'install',$RuntimeDirectory,$StageDirectory)
if($Apply){$arguments+='--apply'}
$report=python @arguments
if($LASTEXITCODE-ne0){throw 'Upgrade refused/failed; inspect the conflict/error above and retained stage.'}
$report|Set-Content -LiteralPath (Join-Path $StageDirectory 'UPGRADE-RESULT.json') -Encoding UTF8
$report|Write-Output
if(!$Apply){Write-Host 'Prepared only; existing runtime unchanged. Review UPGRADE-RESULT.json. To apply this exact stage, close the game first:';Write-Host ('python "'+$helper+'" install "'+$RuntimeDirectory+'" "'+$StageDirectory+'" --apply')}
