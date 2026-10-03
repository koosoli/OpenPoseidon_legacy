# Occupied helicopter rain performance diagnostic

The installed runner is scripts/Test-RainHelicopterPerformance.ps1. It requires the
root-owned build containing the read-only frame_performance seam and all 88 timer
regions. Source helper checks do not establish runtime admission or performance.

Run its source-only admission/parser falsifiers first:

~~~powershell
& scripts/Test-RainHelicopterPerformance.ps1 -SelfTest
~~~

The device/runtime owner runs the installed campaign through the existing serial
game lock, with the exact installed -ExpectedCommit. The runner refuses an existing
game, verifies the provenance stamp and executable/DLL hashes before and after each
arm, and keeps a private Nogova .noe mission and graphics profile. It restores all
changed process environment variables. Successful admission requires normal owned
process exit, exit code zero and the common capture lifecycle scan. Forced cleanup
of its own failed process never counts as success; foreign processes are preserved.

## What the default pair exercises

Both fresh processes mount original stock Nogova through the ordinary mission
loader. The actual stock SoldierWB player occupies the stock UH60 driver seat.
The engine spools on actual ground first. One paused pre-measurement relocation
establishes the requested initial X/Z/altitude, and a brief ordinary Q input
refreshes the stock keyboard height helper. The aircraft settles under physics
with auto-hover. Its measured initial velocity must be at most 2 m/s.

The ordinary held ALT/W controls then fly the helicopter. No position, velocity,
RPM, height-helper or camera writes occur during its forward flight. The stock
external chase camera moves with the aircraft. Diagnostics gate actual player
authority, resolved helicopter input, held forward request, camera effect and
distance, operating RPM, airworthiness, altitude and bounded velocity. At least
50 m of actual displacement and 20 simulation seconds are required. The default
measurement is 25 seconds after a five-second physical forward-flight lead-in.

Both arms request full overcast, zero fog, zero wind, the same date/time, mission,
profile and rendering geometry. Every measured actual weather sample requires
overcast at least .98; matched elapsed-time dry/rain pairs reject an overcast
difference above .01. Dry has actual liquid rain at most .001. Rain uses ordinary
setRain, automatic particle density, real liquid weather at least .25 and real
rain-water input accumulation. There is no forced-rain lighting hook or fabricated
wetness/deposit. Positive live and drawn RainVolume trace rows are required.
Water budgets before and after flight retain real history.

The same requested physical recipe does not guarantee exactly equal trajectories.
The result saves actual X/Z, altitude, three velocity components and input
snapshots. It pairs observations by elapsed route time (within two seconds), emits
their actual differences, and refuses poor overlap: 95th-percentile X/Z difference
over 100 m, altitude difference over 30 m or velocity difference over 20 m/s.
Actual terrain/object visibility ranges are recorded separately; their rain-driven
differences remain part of the workload. Two runs provide a bounded diagnostic,
not repeated statistical performance acceptance or exact scene parity.

## Timing interpretation

triPerfCapture provides the primary ordered frame totals and twelve phase
summaries. The parser requires one complete begin/end block, contiguous offsets,
the exact frame count, all phase names and zero dropped frames. Frame mean, median,
95th percentile and maximum derive from these actual captured CPU frame totals.

The runner polls frame_performance at most once per wall second. Its GPU and
CPU-encoding rows are latest nonblocking publications; the seam exposes no
completed GPU frame identity or timing age. Different getters can describe
different frames, and consecutive samples can repeat the same publication.
Published region output therefore has snapshot count/median/min/max only. It
does not provide GPU unique-frame percentiles, FPS, freshness bounds, same-frame
CPU/GPU correlation or a GPU completion witness. Negative timings remain
unavailable rather than zero cost. Timestamp capability is recorded; unsupported
GPU timing still permits the explicitly CPU-only completed frame capture.

The completed CPU ring contains at most 256 frames and overlaps between polls.
Its raw snapshots are retained, while triPerfCapture owns measured-window CPU
quantiles. Region nesting metadata is retained. The script never sums nested
regions or adds the whole-frame envelope to its children. Weather near/far
cull/draw names occupy indices 84/85/86/87 respectively.

The raw harness protocol, state observations, published snapshots, logs, actual
frame totals, weather/input histories, water budgets and paused route-end screenshot
are saved beneath build/rain-helicopter/. Query cost and once-per-second trace
overhead are part of both measured arms. Moving weather-map and land-rotor traces
are disabled; fine water owners are explicitly off. Weather draw encode timing
must be present in rainy samples. It proves recorded work, not image readiness.
The screenshot is bounded view evidence; it does not prove every aircraft pixel
or all-map renderer behavior.

## Optional diagnostic control

-IncludeFarCoverOff adds another fresh Dry/Rain pair using the existing production
control WGR_WEATHER_COVER_FAR=0. Startup diagnostics must prove OFF and no far-map
publication. Default arms clear the inherited control and require the default-on
startup trace. This control is a diagnostic only; it does not change production
defaults. Its different visibility workload and liquid history are retained.
No water-render-off ablation is supplied because no authoritative production
control was established. Water draw/compute regions remain individually visible
in the ordinary timing snapshots.

Runtime and device acceptance remain pending until the build/deployment owner
executes the campaign and inspects its actual admission/result artifacts.
