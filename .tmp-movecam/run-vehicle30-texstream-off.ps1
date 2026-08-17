# A/B control for run-vehicle30-texstream.ps1: same binary, texture prepare and model parking
# both off => byte-for-byte the daf9223 admission behaviour (async parse still on). The split
# timers still run, so this arm is where the untouched create split is measured:
#   grep "Object stream cold model" .tmp-movecam/out/everon-vehicle30-texstream-off/run-01.log
#   -> ... create=C [reg=.. tex=.. (up=N prep=0) ... park=miss]
# Score:  python scripts/farfield-frametimes.py .tmp-movecam/out/everon-vehicle30-texstream-off/run-01.log
& "C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.claude\worktrees\texstream\scripts\farfield-movecam.ps1" -Label everon-vehicle30-texstream-off -Speed 30 `
  -Mission "D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\dev-missions\devtest-dawn.abel" `
  -World "C:\Users\mail\AppData\Local\Temp\rf\world\ev_DALL.wrp" `
  -Start @(5200,7000) -StartY 220 -GroundY 171.6 -Azimuth 45 -Elevation -8 `
  -WorldHour 10 -SettleSeconds 130 -StaticSeconds 45 -MoveSeconds 150 -Width 1600 -Height 900 `
  -Env @{ WGR_OBJECT_STREAM_TEXTURE_PREPARE = '0'; WGR_GPU_MODEL_PARK = '0' }
