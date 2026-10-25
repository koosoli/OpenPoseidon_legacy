# SEA-RAIN A/B: the Tidewater sea in full rain with WGR_SEA_RAIN off (A) and on (B), same binary, same
# poses, same seed. Writes one PNG per arm and pose plus each run's "Water GPU ms" rows (draw = the
# water shader), into build/sea-rain/. Arms alternate per pose so frame-to-frame drift hits both.
# Run under the game lock:  LOCK_OWNER=sea-rain bash scripts/with-game-lock.sh powershell -File scripts/Test-SeaRain.ps1 -Exe <dir>\OpenPoseidon.exe
param([Parameter(Mandatory)][string]$Exe, [int]$Rounds=2,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'A game instance is already running'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/sea-rain'
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=-1;`nvsync=0;`nfpsCap=60;`n")
$env:POSEIDON_WIND_OVERRIDE='6 90 0'
$env:WGR_AUTO_EXPOSURE='0'
$env:WGR_EXPOSURE='1'
$mission=Join-Path $root 'dev-missions/sea-rain.noe'
# x z y(absolute) azimuth elevation: close (rings), mid (rings fading into roughness), far (glitter/sheen)
$poses=[ordered]@{ near=@('13000','3000','2.2','90','-28'); mid=@('13000','3000','6','90','-10'); far=@('13000','3000','12','90','-3') }
foreach ($round in 1..$Rounds) {
  foreach ($pose in $poses.Keys) {
    foreach ($arm in @('A-off','B-on')) {
      $env:WGR_SEA_RAIN=if ($arm -eq 'B-on') {'1'} else {'0'}
      $name="$pose-$arm-r$round"
      $capture=Join-Path $output "$name.png"
      $log=Join-Path $output "$name.log"
      $f=$poses[$pose]
      $p=Start-Process -FilePath $Exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--test-mission',('"'+$mission+'"'),'--test-world-freefly',$f[0],$f[1],$f[2],$f[3],$f[4],'--auto-screenshot',('"20s:'+$capture+'"'),'--log-file',('"'+$log+'"'))
      $null=$p.Handle
      try {
        if (!$p.WaitForExit(240000) -or $p.ExitCode -ne 0) {throw "Sea rain capture $name failed"}
        if (!(Test-Path -LiteralPath $capture)) {throw "Missing capture $name"}
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw "Renderer error in $name"}
      } finally { if (!$p.HasExited) {$p.Kill()} }
      $draw=@(Select-String -LiteralPath $log -Pattern 'Water GPU ms:.*draw=([0-9.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
      $sorted=$draw | Sort-Object
      $median=if ($sorted.Count) {$sorted[[int][math]::Floor($sorted.Count/2)]} else {-1}
      Write-Host ("{0}  water draw median {1:N3} ms over {2} rows" -f $name, $median, $draw.Count)
    }
  }
}
