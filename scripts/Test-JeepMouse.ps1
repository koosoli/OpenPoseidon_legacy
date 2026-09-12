param([int]$Port=50193, [IO.StreamReader]$Reader, [IO.StreamWriter]$Writer, [string]$LogPath='')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Run only against the owned, game-locked jeep-manual fixture'}
$client=$null
try {
    if (!$Reader -or !$Writer) {
        $client=[Net.Sockets.TcpClient]::new('127.0.0.1',$Port)
        $stream=$client.GetStream(); $stream.ReadTimeout=10000
        $reader=[IO.StreamReader]::new($stream)
        $writer=[IO.StreamWriter]::new($stream); $writer.AutoFlush=$true
    }
    function Send($q) {
        $writer.WriteLine(($q | ConvertTo-Json -Compress))
        do {$line=$reader.ReadLine()} while ($line -and $line -notmatch '"ok"')
        if (!$line) {throw 'Harness disconnected'}
        if ($LogPath) {Add-Content -LiteralPath $LogPath -Value $line}
        $response=$line | ConvertFrom-Json
        if (!$response.ok) {throw $line}
        return $response
    }
    function SeekPose([bool]$aiming,[int]$dx) {
        # Normal boarding preserves the previous view, unlike the old fixture
        # which started beside the firing arc. Allow a complete mouse sweep.
        for ($i=0;$i -lt 360;$i++) {
            $null=Send @{cmd='mouse_motion';dx=$dx;dy=0}
            Start-Sleep -Milliseconds 150
            $pose=Send @{cmd='crew_pose_probe'}
            if (($aiming -and $pose.cargoWeaponLineClear) -or
                (!$aiming -and $pose.primaryMove -eq 'opjeepidle')) {
                Write-Host "Mouse aiming=$aiming reached after $($i+1) events (dx=$dx)"
                return
            }
        }
        throw "Mouse did not reach requested aiming state: $aiming"
    }
    function Shoot {
        $before=[int](Send @{cmd='eval';code='ffvShots'}).result
        $null=Send @{cmd='mouse_button';button=1;down=$true}
        Start-Sleep -Milliseconds 200
        $null=Send @{cmd='mouse_button';button=1;down=$false}
        $after=[int](Send @{cmd='eval';code='ffvShots'}).result
        return $after-$before
    }
    # No crew_cursor/camera override: mouse events must drive the normal UI.
    SeekPose $true 8
    if ((Shoot) -lt 1) {throw 'First mouse-aimed shot failed'}
    for ($turn=0; $turn -lt 8; ++$turn) {
        $null=Send @{cmd='mouse_motion';dx=100;dy=0}
        Start-Sleep -Milliseconds 300
        if ((Shoot) -lt 1) {throw 'Free cargo aiming lost firing permission during mouse sweep'}
    }
    SeekPose $true -8
    if ((Shoot) -lt 1) {throw 'Returning to rifle arc failed'}
    $null=Send @{cmd='key';sc=21;hold=$true}
    $deadline=[DateTime]::UtcNow.AddSeconds(2)
    do {
        Start-Sleep -Milliseconds 50
        $pose=Send @{cmd='crew_pose_probe'}
    } while (!$pose.reloadInProgress -and [DateTime]::UtcNow -lt $deadline)
    $null=Send @{cmd='key_up';sc=21}
    $seenReload=$false
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 100
        $pose=Send @{cmd='crew_pose_probe'}
        if ($pose.primaryMove -eq 'opjeepreload') {
            if (!$pose.weaponsDisabled) {throw 'Reload left firing enabled'}
            $seenReload=$true
        }
    } while ((!$seenReload -or $pose.reloadInProgress -or !$pose.loaded) -and [DateTime]::UtcNow -lt $deadline)
    if (!$seenReload -or $pose.reloadInProgress -or !$pose.loaded) {throw 'Normal-menu keyboard reload failed'}
    SeekPose $true -8
    if ((Shoot) -lt 1) {throw 'Mouse firing after keyboard reload failed'}
    Write-Output 'PASS: normal menu/mouse free cargo fire, keyboard reload and firing after reload.'
} finally {if ($client) {$client.Dispose()}}
