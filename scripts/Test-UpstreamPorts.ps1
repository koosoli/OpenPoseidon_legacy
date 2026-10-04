[CmdletBinding()]
param(
 [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit,
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [string]$DataDir='D:/SteamLibrary/steamapps/common/Arma Cold War Assault Demo'
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
function Require([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Pair {
 $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw
 Require ($stamp.Contains($ExpectedCommit)) 'Installed source differs from requested checkpoint.'
 @{stamp=$stamp;exe=(Get-FileHash -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash -LiteralPath (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner session is preserved.'
Require (Test-Path -LiteralPath (Join-Path $DataDir 'demo/demo.wrp')) 'Imported Demo fixtures require installed Demo data; no data is downloaded.'
$before=Pair
$out=Join-Path $root ('build/upstream-runtime/'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $out|Out-Null
$fixtures=@('tests/integration/scripting/drop_particle_height.test.sqf',
 'tests/integration/scripting/fired_nearest_object.test.sqf',
 'tests/integration/ingame/vehicles/vehicle_turbo.test.sqf',
 'tests/integration/ui/main_menu/animated_notebook_hover_audio.test.sqf')
$keys=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
 'POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_FLAKES',
 'WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','POSEIDON_AUTOMATIC_RAGDOLL','POSEIDON_LOD_FIX')
$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{passed=$false;installed=$before;dataDirectory=$DataDir;scriptSha256=(Get-FileHash $PSCommandPath).Hash;fixtures=@();scope='Serial imported gameplay fixtures on matched installed executable/renderer with existing Demo data, isolated Trident profiles; no full multiplayer, Linux or all-map certification.'}
$p=$null
try {
 foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$null,'Process')}
 foreach($fixture in $fixtures){
  Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Game ownership conflict.'
  Require (((Pair)|ConvertTo-Json -Compress)-ceq($before|ConvertTo-Json -Compress)) 'Installed pair changed.'
  $name=[IO.Path]::GetFileNameWithoutExtension($fixture)
  $caseOut=Join-Path $out $name;New-Item -ItemType Directory -Force $caseOut|Out-Null
  $log=Join-Path $caseOut 'engine.log'
  $arguments=@('test',('"'+$fixture+'"'),'--game-dir',('"'+$GameDir+'"'),
   '--data-dir',('"'+$DataDir+'"'),
   '--render','wgpu','--jobs','1','--retries','0','--output-dir',('"'+$caseOut+'"'),
   '--game-arg','--log-file','--game-arg',('"'+$log+'"'))
  $p=Start-Process (Join-Path $root 'target/debug/tri.exe') -WorkingDirectory $root -WindowStyle Hidden -ArgumentList $arguments -RedirectStandardOutput (Join-Path $caseOut 'tri.stdout') -RedirectStandardError (Join-Path $caseOut 'tri.stderr') -PassThru
  $null=$p.Handle
  $deadline=[DateTime]::UtcNow.AddSeconds(240)
  while(!$p.HasExited){Require ([DateTime]::UtcNow-lt$deadline) "Owned fixture timeout: $fixture";Start-Sleep -Milliseconds 250}
  $p.WaitForExit()
  Require ($p.ExitCode-eq0) "Trident fixture failed: $fixture; see $caseOut"
  Require (Test-Path -LiteralPath $log) 'Missing actual installed engine log.'
  Require (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) 'Game did not report completed shutdown.'
  Require (!(Select-String -LiteralPath $log -Pattern 'Validation Error|DeviceLost|uncaught exception|unhandled exception|panicked at' -Quiet)) 'GPU or unhandled runtime failure.'
  Require (!(Get-Process OpenPoseidon -ErrorAction SilentlyContinue)) 'Fixture leaked its game process.'
  $result.fixtures+=@{path=$fixture;sha256=(Get-FileHash (Join-Path $root $fixture)).Hash;exitCode=$p.ExitCode;output=$caseOut;engineLogSha256=(Get-FileHash $log).Hash}
  $p=$null;Write-Output "PASS $fixture"
 }
 $result.after=Pair
 Require (($result.after|ConvertTo-Json -Compress)-ceq($before|ConvertTo-Json -Compress)) 'Installed pair changed during smoke.'
 $result.passed=$true
} catch {$result.error=$_.Exception.Message;throw}
finally {
 if($p-and!$p.HasExited){$result.ownedTimeoutCleanup=$true;$p.Kill($true);$null=$p.WaitForExit(10000)}
 foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
 $result|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $out 'result.json')
 Write-Output $out
}
