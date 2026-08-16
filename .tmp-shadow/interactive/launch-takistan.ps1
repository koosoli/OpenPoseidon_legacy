# INTERACTIVE Takistan session for the owner to fly to a broken-shadow interior.
# Default (FIXED) cascade gate, gate trace on. No auto-screenshot: this session is flown by hand.
$game = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
$log  = "$PSScriptRoot\takistan-fixed.log"
Remove-Item $log -ErrorAction SilentlyContinue
$env:WGR_SHADOW_GATE_TRACE = '1'
# Legacy arm is a separate launch; set this to '1' to reproduce the OLD gate for comparison.
if ($env:ARM_LEGACY -eq '1') { $env:WGR_SHADOW_LEGACY_GATE = '1'; $log = "$PSScriptRoot\takistan-legacy.log" }
$a = @(
  '--render wgpu','--window','--dev','--width 1600','--height 900',
  "--test-mission `"$game\dev-missions\devtest-dawn.abel`"",
  '--test-world "C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp"',
  '--addon-root "C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons"',
  '--test-world-freefly 6770.69 6099.78 312.86 230.7 -6.8',
  '--test-world-hour 8',
  "--log-file `"$log`"",'--log-level info'
) -join ' '
cmd /c "start `"takistan-interactive`" /D `"$game`" `"$game\ColdWarAssault.exe`" $a"
Write-Host "launched; log -> $log"
