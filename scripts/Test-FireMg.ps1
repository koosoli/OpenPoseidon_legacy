# FIRE-MG A/B: two campfires and a burning truck at dusk, the fire's pressure solved by 24 Jacobi
# sweeps (WGR_FIRE_MG=0, today) or by 1 / 2 multigrid V-cycles. Same binary, same poses. Writes one
# PNG per arm and pose plus each run's whole-frame GPU time ("Shadow GPU ms: ... frame=") and the
# fire box count into build/fire-mg/. Arms alternate per pose so drift hits all of them.
# Run under the game lock:  LOCK_OWNER=fire-mg bash scripts/with-game-lock.sh powershell -File scripts/Test-FireMg.ps1 -Exe <dir>\OpenPoseidon.exe
param([Parameter(Mandatory)][string]$Exe, [int]$Rounds=2, [string[]]$Arms=@('0','1','2'),
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'A game instance is already running'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/fire-mg'
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=-1;`nvsync=0;`nfpsCap=0;`n")
$env:POSEIDON_WIND_OVERRIDE='2 90 0'
$mission=Join-Path $root 'dev-missions/fire-mg.eden'
# x z y(absolute) azimuth elevation: close on the fires, and wide with the truck's column
$poses=[ordered]@{ close=@('5092','4046','19','180','-8'); wide=@('5091','4062','25','180','-6') }
foreach ($round in 1..$Rounds) {
  foreach ($pose in $poses.Keys) {
    foreach ($arm in $Arms) {
      $env:WGR_FIRE_MG=$arm
      $name="$pose-mg$arm-r$round"
      $capture=Join-Path $output "$name.png"
      $log=Join-Path $output "$name.log"
      $f=$poses[$pose]
      $p=Start-Process -FilePath $Exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--test-mission',('"'+$mission+'"'),'--test-world-freefly',$f[0],$f[1],$f[2],$f[3],$f[4],'--auto-screenshot',('"30s:'+$capture+'"'),'--log-file',('"'+$log+'"'))
      $null=$p.Handle
      try {
        if (!$p.WaitForExit(240000) -or $p.ExitCode -ne 0) {throw "Fire capture $name failed"}
        if (!(Test-Path -LiteralPath $capture)) {throw "Missing capture $name"}
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw "Renderer error in $name"}
      } finally { if (!$p.HasExited) {$p.Kill()} }
      $frame=@(Select-String -LiteralPath $log -Pattern 'Shadow GPU ms:.*frame=([0-9.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
      $sorted=$frame | Sort-Object
      $median=if ($sorted.Count) {$sorted[[int][math]::Floor($sorted.Count/2)]} else {-1}
      $boxes=(Select-String -LiteralPath $log -Pattern 'Fire: \d+ reported, (\d+) boxes' | Select-Object -Last 1 | ForEach-Object { $_.Matches[0].Groups[1].Value })
      Write-Host ("{0}  frame GPU median {1:N3} ms over {2} rows, fire boxes {3}" -f $name, $median, $frame.Count, $boxes)
    }
  }
}
