param([switch]$DayZ)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {
 $ps=Join-Path $PSHOME $(if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'})
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$ps.Replace('\','/'),'-NoProfile','-File',$PSCommandPath)
 if ($DayZ) {$argv+='-DayZ'}
 $env:LOCK_OWNER='headless streaming verification'
 try {& 'C:/Program Files/Git/bin/bash.exe' @argv; if ($LASTEXITCODE) {throw "Headless check exited $LASTEXITCODE"}} finally {Remove-Item Env:LOCK_OWNER}
 return
}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root (('build/stream-residency/headless-'+$(if ($DayZ) {'dayz'} else {'legacy'})+'-')+(Get-Date -Format yyyyMMdd-HHmmss))
$mission=Join-Path $out 'stream_headless.eden'
New-Item -ItemType Directory -Force $mission | Out-Null
$source=Get-Content (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Raw
$source.Replace('id=0;','id=0; text="streamProbe";') | Set-Content (Join-Path $mission 'mission.sqm')
@"
~2
streamProbe setDammage 0.25
~3
logInfo format ["STREAM_ASSERT_%1",triAssertEq [time > 3,true]]
logInfo format ["STREAM_ASSERT_%1",triAssertEq [alive streamProbe,true]]
logInfo format ["STREAM_ASSERT_%1",triAssertEq [(getDammage streamProbe > 0.24) && (getDammage streamProbe < 0.26),true]]
logInfo format ["STREAM_HEADLESS_OK time=%1 damage=%2",time,getDammage streamProbe]
streamMapObject = object 83903272
$(if ($DayZ) { 'logInfo format ["STREAM_ASSERT_%1",triAssertEq [!(isNull streamMapObject),true]]' })
$(if ($DayZ) {'streamMapObject setDammage 0.25'})
~1
$(if ($DayZ) { 'logInfo format ["STREAM_ASSERT_%1",triAssertEq [streamMapObject == object 83903272,true]]' })
$(if ($DayZ) { 'logInfo format ["STREAM_ASSERT_%1",triAssertEq [(getDammage streamMapObject > 0.24) && (getDammage streamMapObject < 0.26),true]]' })
logInfo format ["STREAM_HEADLESS_PLACEMENT83903272_PRESENT=%1",!(isNull streamMapObject)]
triEndTest
"@ | Set-Content (Join-Path $mission 'init.sqs')
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
$env:WGR_OBJECT_STREAM_PBO='0'
$env:WGR_OBJECT_STREAM_PBO_TEXTURES='0'
@{head=(& git -C $root rev-parse HEAD); serverHash=(Get-FileHash (Join-Path $root 'dist/x64-win-rwdi/PoseidonServer.exe')); missionHash=(Get-FileHash (Join-Path $mission 'mission.sqm')); initHash=(Get-FileHash (Join-Path $mission 'init.sqs'))} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'provenance.json')
$worldArgs=@()
if ($DayZ) {$worldArgs=@('--test-world',(Resolve-Path (Join-Path $root '../../../packages/dayz-compat/world/chernarusplus/chernarusplus.wrp')).Path,'--addon-root','D:\SteamLibrary\steamapps\common\DayZ\Addons')}
& (Join-Path $root 'scripts/Start.ps1') -App Server -LogFile (Join-Path $out 'engine.log') --simulate $mission --duration 45 --stats 2 --private --bind-address 127.0.0.1 --port 24851 --nosound @worldArgs *> (Join-Path $out 'console.log')
if ($LASTEXITCODE) {throw "Headless test exited $LASTEXITCODE; $out"}
$log=Get-Content (Join-Path $out 'engine.log') -Raw
if ($log -notmatch 'STREAM_HEADLESS_OK' -or $log -match 'STREAM_ASSERT_FAIL|UNHANDLED EXCEPTION|panicked at' -or ([regex]::Matches($log,'STREAM_ASSERT_OK').Count -ne $(if ($DayZ) {6} else {3}))) {throw "Headless verification failed; $out"}
Write-Output "Headless residency evidence: $out"
