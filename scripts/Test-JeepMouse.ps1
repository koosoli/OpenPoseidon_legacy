param([int]$Port=50193)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Run only against the owned, game-locked jeep-manual fixture'}
$client=[Net.Sockets.TcpClient]::new('127.0.0.1',$Port)
try {
    $stream=$client.GetStream(); $stream.ReadTimeout=10000
    $reader=[IO.StreamReader]::new($stream)
    $writer=[IO.StreamWriter]::new($stream); $writer.AutoFlush=$true
    function Send($q) {
        $writer.WriteLine(($q | ConvertTo-Json -Compress))
        do {$line=$reader.ReadLine()} while ($line -and $line -notmatch '"ok"')
        if (!$line) {throw 'Harness disconnected'}
        $response=$line | ConvertFrom-Json
        if (!$response.ok) {throw $line}
        return $response
    }
    function SeekPose([bool]$aiming,[int]$dx) {
        for ($i=0;$i -lt 90;$i++) {
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
    SeekPose $false 8
    if ((Shoot) -ne 0) {throw 'Shot escaped while looking outside rifle arc'}
    SeekPose $true -8
    if ((Shoot) -lt 1) {throw 'Returning to rifle arc failed'}
    Write-Output 'PASS: normal relative mouse entered, left and returned to rifle arc; actual shots only inside.'
} finally {$client.Dispose()}
