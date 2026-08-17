# Texture streaming + model parking -- the arm under test. Same drive as
# run-vehicle30-async.ps1 (Reforger Everon, 30 m/s, 150 s move phase); engine defaults, i.e.
# WGR_OBJECT_STREAM_TEXTURE_PREPARE=1 (budget 512 MB, ttl 30 s) and WGR_GPU_MODEL_PARK=256.
#
# Score:  python scripts/farfield-frametimes.py .tmp-movecam/out/everon-vehicle30-texstream/run-01.log
# A/B:    run-vehicle30-texstream-off.ps1 (same binary, WGR_OBJECT_STREAM_TEXTURE_PREPARE=0
#         WGR_GPU_MODEL_PARK=0 => exactly the daf9223 behaviour)
# Proof it fired (grep the log):
#   "Object stream prepare: 2 workers armed for N models ... texture prepare=on (budget=512 MB ttl=30 s)"
#   per cold model: "Object stream cold model: <p3d> ... create=C [reg=.. mesh=.. sect=.. tex=..
#        (read=.. create=.. fb=.. alpha=.. up=N prep=N) ffi=.. prox=.. other=.. park=hit|stale|miss] total=.."
#   cumulative:    "Modern object create split: ... texprep worker=Nchains/M MB/T ms consumed=X/Y ...
#        park hit=H stale=S"
# Read `prep=` and `park=` first: prep/up is the share of uploads served from worker-prepared
# chains (the texture read+LZO removed from the frame), park=hit rows paid no RegisterGpuModel
# at all. If both stay ~0 while `tex`+`reg` dominate `create`, the change is a measured null.
& "C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.claude\worktrees\texstream\scripts\farfield-movecam.ps1" -Label everon-vehicle30-texstream -Speed 30 `
  -Mission "D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\dev-missions\devtest-dawn.abel" `
  -World "C:\Users\mail\AppData\Local\Temp\rf\world\ev_DALL.wrp" `
  -Start @(5200,7000) -StartY 220 -GroundY 171.6 -Azimuth 45 -Elevation -8 `
  -WorldHour 10 -SettleSeconds 130 -StaticSeconds 45 -MoveSeconds 150 -Width 1600 -Height 900
