# farfield-bench — repeatable A/B GPU benchmarking

Two files:

| file | what it does |
|---|---|
| `scripts/farfield-bench.ps1` | runs one **arm** (a label + a set of env vars) N times, writes one metrics JSON + PNG + log per repeat |
| `scripts/farfield-compare.py` | reads two or more arms and prints the comparison table — GPU regions **and** the CPU frame phases |

Nothing here builds the game. It measures **whatever binary is installed** in the game
directory, which is the only way to benchmark while somebody else is mid-edit in `engine/`.

---

## Read this first: why `--capture-metrics` writes nothing on its own

`--capture-metrics <path>` is written **only by a screenshot event**. All four call sites of
`WriteCaptureMetrics` in `apps/cwr/Game/GameApplication.cpp` sit immediately after
`GEngine->Screenshot()`: the menu screenshot, the `--test-mission --test-type screenshot`
capture, and `--auto-screenshot` (in both game loops). Nothing writes it on exit, on close,
or on a timer.

And **`--duration` is not a client option at all** — `_simulateDuration` is read only by
`apps/cwr/Server/ServerApplication.cpp`. So

```
ColdWarAssault.exe --duration 45 --capture-metrics out.json      # writes NOTHING, ever
```

runs until something kills it and produces no JSON. That is not a focus-loss bug; there is
no code path from that command line to a written file. `farfield-bench.ps1` always pairs
`--capture-metrics` with a screenshot trigger, so the failure cannot recur.

---

## Run an A/B

```powershell
# arm A
.\scripts\farfield-bench.ps1 -Label legacy -Repeats 3 `
    -Env @{ WGR_LOD_GOVERNOR_RANGE = '16'; WGR_OBJECT_STREAM_MS_PER_UPDATE = '0' }

# arm B
.\scripts\farfield-bench.ps1 -Label tiered -Repeats 3 `
    -Env @{ WGR_LOD_GOVERNOR_RANGE = '4';  WGR_OBJECT_STREAM_MS_PER_UPDATE = '4' }

# compare
python .\scripts\farfield-compare.py `
    --arm legacy=.tmp-farfield-bench\legacy `
    --arm tiered=.tmp-farfield-bench\tiered
```

Default output root is `.tmp-farfield-bench\<label>\` (override with `-Out`). Each repeat
leaves `run-NN.json` (metrics), `run-NN.png`, `run-NN.log`, `run-NN.meta.json`
(status/env/arguments/contention) plus one `arm.json` manifest for the whole arm.

Default mission is `tests/perf/missions/perf_abel.abel`. The other four in that folder
(`perf_combat.eden`, `perf_field.eden`, `perf_town.noe`, `perf_water.eden`) are drivable with
`-Mission tests/perf/missions/perf_town.noe`.

**Do not measure while anything else is building.** A background compile inflates the
simulation roughly **fifteenfold** on `perf_combat` — measured, same build, same scene, same
110 s warm-up:

| | with two subagents compiling | idle |
|---|---:|---:|
| frame mean | 372 ms | **51.6 ms** |
| `sim:step` per frame | 286 ms | **18.2 ms** |
| tick rate | 40 of 60 | **60–63 of 60** |
| catch-up steps discarded per second | 23–35 | **0** |

**`dist/` staging fails SILENTLY while the game is running.** CMake stages
`wgpu_renderer.dll` into `dist/` after every link; a live game instance holds that DLL, the
copy fails, and **the exe in `dist/` keeps its old timestamp** while the build reports
success. Measured 2026-09-01: three consecutive "successful" builds, and `dist` never moved
off 16:06 — two measurement rounds were spent explaining behaviour of code that was not in
the binary. The build DOES say `Error copying file ... wgpu_renderer.dll`, but it has no
colon after "Error", so the usual `grep -E "error:"` filter hides it. Check the exe
timestamp, or grep the binary for a string you just added.

**`frame_max` is the screenshot, not a hitch.** `FlushPendingScreenshot()` is inside
`NextFrame`, so the readback lands in that frame's `swap`; the harness takes one at the end of
the run and the 256-frame phase ring closes there too, so the capture frame is inside the ring
of every measurement. Measured: 298.5 ms of which 271.3 is the readback. **Read p95 and below;
the maximum is the shutter.** (PERF-018's correction.)

**And do not touch the window either.** A behaviour recording
(`POSEIDON_AI_TIMELINE`, PERF-017) diverged at tick 306 between two runs of the same build
with the same seed, because the camera was nudged while one of them was running. The camera is
not obviously part of the simulation and it is; a run being measured must be left alone, not
merely left unbuilt.

That is enough to invent findings out of nothing: it produced a whole decision document
claiming the AI stage cost 21–34 ms/tick and that the world ran at two thirds speed, both of
which are artefacts (see the correction banner on `PERF-013`). The GPU-side figures move far
less, which makes the trap worse — a loaded run looks plausible rather than obviously broken.

Check before starting, not after a suspicious result: `tasklist | grep -iE "clang|ninja|rustc"`
must be empty. The same applies to `scripts\Check-FrameEquivalence.ps1`, which under load
reported a phantom regression in three innocent commits.

**Choose the mission for the change you are measuring, and do not let the default choose for
you.** These scenes are not interchangeable, and the default is the most GPU-bound of them:

| | CPU frame in `swap` | simulation-side CPU |
|---|---:|---:|
| `perf_abel` (the default) | **91%** | 0.15 ms |
| `perf_town` | 81% | 0.46 ms |
| `perf_combat` | ~50% | 17–24 ms |

So `perf_abel` measures the GPU and almost nothing else: a change to CPU-side work — the
simulation, the object stream, threading, anything that would be hidden behind a GPU wait —
reads as noise there by construction. `perf_combat` is the only one of the five with real
simulation load (AI, vehicles, radio traffic, particles) and it is where CPU work shows.

This is not hypothetical. Every performance figure in this repository's history was taken on
`perf_abel`, including a whole grass campaign that belonged there — and PERF-012 then measured
that render/simulation overlap is worth 34–42% of a combat frame and **1.3%** of an abel
frame. Measuring step 9 on the default would have concluded it does nothing. See
`design notes`.

A caution that goes with them: `perf_combat` streams for a long time on Everon. Its frame is
still dominated by object admission at 900 frames; the run has to reach roughly 110 s (or the
`Renderer mutation ops` line has to read zero) before "steady" means anything. `-WarmupSeconds
110` with a raised `-TimeoutSeconds` gets there; the default 150 s watchdog does not.

### Against a substituted world

```powershell
.\scripts\farfield-bench.ps1 -Label everon-legacy -Repeats 3 `
    -Mission "D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\dev-missions\devtest-dawn.abel" `
    -World   "C:\Users\<you>\AppData\Local\Temp\rf\world\ev_DALL.wrp" `
    -Freefly 5200,7000,220,45,-8 -WorldHour 10 -WarmupSeconds 35 `
    -Env @{ WGR_OBJECT_STREAM_MAX_OBJECTS = '200000' }
```

Passing `-World` switches to **world mode**: `--test-world` / `--test-world-freefly` /
`--test-world-hour`, a wall-clock `--auto-screenshot "<S>s:<png>"` trigger, and no `--check`.
The freefly pose is `X Z Y azimuth elevation`, exactly as the engine logs it back.

**Both arms of an A/B must use the same pose, the same warmup and the same mission.** The
label only names the env vars; nothing else is allowed to differ.

---

## Environment levers

Set them through `-Env @{ ... }` — the script applies them to the child process only and
restores the parent's values afterwards, so two arms in one shell cannot leak into each other.

| variable | default | legacy value | what it does |
|---|---|---|---|
| `WGR_LOD_GOVERNOR_RANGE` | `4` | `16` | bounds how far the LOD governor may travel from the ideal LOD (`engine/Poseidon/World/Scene/Scene.cpp`) |
| `WGR_OBJECT_STREAM_MS_PER_UPDATE` | `4` | `0` | time budget in ms per object-admission update; `0` pins admission to the legacy per-frame batch (`engine/Poseidon/World/Terrain/LandSave.cpp`) |
| `WGR_OBJECT_STREAM_MAX_OBJECTS` | `20000` | — | residency budget: how many placements may be resident at once (`LandSave.cpp`) |
| `WGR_FAR_TIER` | `1` (on) | `0` (off) | **not yet deployed** — master switch for the far-instance tier; `0` stops the sweep but leaves every other path in place, so both arms come from one binary |
| `WGR_FAR_NEAR_CUTOFF` | `-1` = follow `Scene::GetObjectDrawDistance()` | — | **not yet deployed** — where the far tier starts; an explicit value is a diagnostic override |
| `WGR_FAR_DISTANCE` | `12000` | — | **not yet deployed** — far tier range in metres |
| `WGR_FAR_PIXEL_LIMIT` | `2` | `0` (test off) | **not yet deployed** — minimum projected height in pixels for a far instance to survive |

The `WGR_FAR_*` levers are being written **right now** in this branch
(`engine/WgpuRenderer/EngineWgpu.cpp`, `rust/src/far/`). They are read once at startup, so a
value changed mid-run does nothing. They are **not in the installed binary**, so until a build
is deployed an A/B on them returns a null result meaning "the lever does not exist", not "the
lever does not help". Run the binary check below before believing any far-tier number.

### Check the installed binary actually has a lever before you A/B it

An env var the binary never reads produces two identical arms and a confident wrong
conclusion. One command settles it:

```powershell
$dir  = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
$text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes("$dir\ColdWarAssault.exe"))
foreach ($v in 'WGR_LOD_GOVERNOR_RANGE', 'WGR_OBJECT_STREAM_MS_PER_UPDATE', 'WGR_FAR_TIER') {
    '{0}: {1}' -f $v, $text.Contains($v)
}
```

`False` means the deployed exe predates the lever, whatever the source tree says. Some levers
live in `wgpu_renderer.dll` instead, so run the same check against that file too. Measured on
the binary installed at the time of writing: `WGR_OBJECT_STREAM_MAX_OBJECTS` present, the two
governor/streaming levers **absent** (they are newer than the deployment), `WGR_FAR_*` absent.

Scope matters too: `WGR_OBJECT_STREAM_MAX_OBJECTS` is read on the modern-world residency
path, so on a **native OFP world it is inert** — measured, 200000 vs 2000 on `perf_abel.abel`
gave byte-identical object counts (98,684 registered / 579 main / 13,154 triangles in both).

---

## Reading the comparison

```
FRAME BUDGET (ms)
                                         budget200k     budget2k        delta            %
GPU frame total                              30.318       29.356       -0.962        -3.2%
attributed (sum of top-level)                18.674       18.122       -0.552        -3.0%
residual (untimed passes)              11.644 (38%) 11.234 (38%)       -0.410        -3.5%
```

That is the **GPU** frame. Read the CPU section below it before drawing any conclusion from
these three rows — on the imported worlds the GPU is a minority of wall clock, so a GPU-only
delta can be real and still change nothing.

Four rules the comparer enforces, none of them optional:

1. **The cheapest steady frame is quoted, never the mean.** A second process on the GPU
   inflates *every* region by a common factor (RND-033 measured 27.9 ms against 7.6 ms for
   the same scene, cull 2.214 vs 0.449), so the minimum is the only statistic contamination
   cannot push down. One whole repeat is picked — the one with the lowest GPU frame total —
   and every number comes from that single capture. All repeats' frame totals are printed,
   with `*` marking the quoted one, so a wide spread is visible rather than averaged away.
2. **`milliseconds == -1` means "did not run"**, and prints as such. A region that ran in one
   arm and not the other reports its delta as `gated`, because a gate firing is not a
   speed-up and `x - (-1)` is not a measurement.
3. **The residual is a row.** `frame total ~= sum of rows where contained_by == -1` (the
   capture writer emits that rule per row). Roughly half of every frame is in passes with no
   timer region — main-view sky, the cloud march and composite, bloom, godrays, tonemap, UI —
   so the difference between the frame and the attributed sum is reported, not hidden.
4. **A missing measurement is never a zero.** `cpu_frame_phases_ms` absent from an old capture
   prints as absent and says why; it does not default to 0 ms of CPU work.

Useful flags: `--all-regions` (also lists the nested leaves under their containers, which must
never be added to the top-level sum, **and** a second CPU-phase table showing p95), `--top N`,
`--json out.json`, `--include-contended`.

Exit codes: `0` fine, `1` an arm is missing/malformed/has no usable capture, `2` fewer than
two `--arm` values.

---

## The CPU section — the one that decides

`WriteCaptureMetrics` also emits `cpu_frame_phases_ms`, straight out of the always-on
`Poseidon::Dev::FrameProfiler` (`engine/Poseidon/Dev/Diag/FrameProfiler.hpp`). Until it was
added, the CPU side of the frame was visible only through the dev panel or `triPerfStats` —
i.e. only to somebody sitting at the keyboard — so every recorded capture in the repo shows
GPU numbers for a frame that is mostly **not** GPU.

```
CPU FRAME (ms)
                                             legacy       tiered        delta            %
------------------------------------------------------------------------------------------
sampled frames (profiler ring)                  256          256
avg fps                                        8.99        11.90       +2.912       +32.4%
CPU frame (avg)                             111.200       84.000      -27.200       -24.5%
CPU frame (p95)                             148.300      115.200      -33.100       -22.3%
CPU frame (max)                             402.100      348.400      -53.700       -13.4%
GPU frame total (cheapest repeat)            30.403       30.403       +0.000        +0.0%
------------------------------------------------------------------------------------------
>> CPU frame MINUS GPU frame                 80.797       53.597      -27.200       -33.7%
   GPU share of the CPU frame                 27.3%        36.2%
```

**`>> CPU frame MINUS GPU frame` is the headline.** It is the part of the frame no GPU change
can touch. If it dominates, a GPU-only win moves nothing a player can see — RND-033 measured
the GPU at roughly 9% of wall clock on the heavy imported worlds, so a shader that halves a
3 ms pass buys 1.5 ms out of a 110 ms frame. A **negative** value means the frame genuinely is
GPU-bound and GPU work is the right target.

Two windows, not one: the CPU rows are **averages over the profiler ring** (up to 256 frames
of the run), while the GPU row is the single cheapest repeat. The subtraction is a magnitude,
not two simultaneous measurements. The cheapest-repeat rule still selects the repeat by *GPU*
frame total, so the CPU numbers quoted come from whichever repeat won on GPU.

### The phase table

```
CPU PHASES (ms, avg per frame; DESCENDING -- the dominant phase is first)
land:gnd  <<< +OBJECT STREAMING              58.700       31.500      -27.200       -46.3%
land:obj                                     14.200       13.900       -0.300        -2.1%
setup                                        12.400       12.100       -0.300        -2.4%
...
attributed (sum of phase avgs)              109.400       81.100      -28.300       -25.9%
unattributed (frame - phases)            1.800 (2%)   2.900 (3%)       +1.100       +61.1%
```

Sorted **descending by avg**, so the phase to attack is the first row. The twelve phases are
`setup`, `drw:init`, `drw:prep`, `land:gnd`, `land:obj`, `drw:land`, `drw:obj`, `drw:post`,
`hud`, `ai+veh`, `sound`, `swap` — marked sequentially from the frame start, so they partition
the frame and the **averages** are additive. p95 is **not**: different frames peak in different
phases, so a sum of p95s describes a frame that never ran. Only avgs are summed; p95 gets its
own table under `--all-regions`.

`unattributed` is the gap between the frame total and the phase sum — the profiler's last mark
to `EndFrame`. A couple of percent is normal; a large one means a phase boundary is missing.

### `land:gnd` includes object streaming

The tool prints this in a banner and tags the row, and it is not decoration:

> `UpdateModernObjectResidency` is called from `TerrainWgpu::DrawTerrain`, so object admission
> is billed **inside** the terrain phase.

A large `land:gnd` on a heavy world is **object cost, not terrain rasterisation cost**. Read it
as terrain and the next day goes into terrain meshing while the actual cost sits in placement
admission. Cross-check against `objects.registered_instances` and the streaming levers
(`WGR_OBJECT_STREAM_MS_PER_UPDATE`, `WGR_OBJECT_STREAM_MAX_OBJECTS`) before touching terrain.

### Healthy vs unhealthy splits

| shape | reading |
|---|---|
| no phase over ~35% of the frame, `land:gnd` in line with `land:obj` | **healthy** — cost is spread; there is no single win, only broad ones |
| `land:gnd` at 40-60% on a heavy imported world | **streaming-bound**, almost never terrain. Attack residency/admission |
| `setup` dominant | scripts, input, network or landscape sim — not the renderer at all |
| `ai+veh` dominant | simulation, not rendering. A renderer A/B on this world measures nothing |
| `swap` dominant | presentation stall: vsync, or the CPU waiting on a GPU that is genuinely the bottleneck. Check the CPU-minus-GPU row — it will be small |
| CPU frame ≫ GPU frame (GPU share under ~20%) | **CPU-bound.** GPU-side A/Bs on this world are noise-hunting |
| GPU share above ~80% | GPU-bound; the GPU tables above are the ones to read |
| p95 more than ~2× avg on one phase | that phase spikes. An average-only comparison will hide it — check the p95 table (`--all-regions`) |

### Old captures

Captures written before the instrumentation have no such block, and they are still perfectly
good GPU measurements. The comparer prints

```
  cpu_frame_phases_ms absent (binary predates the instrumentation)
```

and names the arms, rather than crashing or — much worse — printing zeros. **Absent means
unrecorded, never zero**: a zero CPU frame is the single most misleading thing this tool could
say about a CPU-bound world. In a mixed A/B (one arm's binary has the block, the other's does
not) the missing arm's cells read `absent`, deltas read `n/a`, and the tool tells you that you
are comparing two different binaries. In `--json`, such an arm gets
`"cpu": {"present": false, "reason": ...}` — null, not zeros.

---

## Failure modes the harness makes loud

* **No JSON** → the run is failed with the reason, and the last 12 log lines are printed.
* **`gpu_timestamps_available: false`** → failed, never reported as zeros. Both the driver
  and the comparer refuse it.
* **`GPU frame total <= 0`** → failed.
* **Another game process already running** → the script refuses to start. Use
  `-AllowConcurrent` to measure anyway; those repeats are stamped `contended: true` in
  `run-NN.meta.json`, the comparer drops them when it can and warns loudly when it cannot.
* **Watchdog** → each repeat is bounded (`-TimeoutSeconds`, default 150 s in mission mode,
  warmup + 180 s in world mode). On expiry the script kills **only the PID it started and
  that PID's children**. Every game process that existed beforehand is recorded and protected;
  the script never kills by process name. RND-033 records capture scripts killing each other's
  runs eight times in one session — that is the mistake this avoids.

* **Negative residual** → the capture's nesting table disagrees with the summing rule (older
  binaries marked the whole-frame envelope as outermost, so it got added to its own parts).
  Both tools now exclude the `GPU frame total` row by name; if a residual still comes out
  negative the comparer says so in place of the number.

## Two things that are not failures but will fool you

* **`-ScreenshotDelay` counts gameplay FRAMES, not seconds** (mission mode). At 30 fps the
  default 10 is a third of a second past the first gameplay frame. Use world mode's
  `-WarmupSeconds` when you need a clock.
* **Warmup vs residency.** Substituted worlds stream objects for 10-20 s and longer at large
  budgets. A 25 s warmup on `ev_DALL.wrp` captured `registered_instances = 6,270` — still
  filling. Raise `-WarmupSeconds` until `objects.registered_instances` plateaus across
  repeats, then keep that number fixed for both arms. Comparing a settled arm against an
  unsettled one measures the warmup, not the change.
