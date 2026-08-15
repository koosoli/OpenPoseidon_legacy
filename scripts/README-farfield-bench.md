# farfield-bench — repeatable A/B GPU benchmarking

Two files:

| file | what it does |
|---|---|
| `scripts/farfield-bench.ps1` | runs one **arm** (a label + a set of env vars) N times, writes one metrics JSON + PNG + log per repeat |
| `scripts/farfield-compare.py` | reads two or more arms and prints the comparison table |

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

Three rules the comparer enforces, none of them optional:

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

Useful flags: `--all-regions` (also lists the nested leaves under their containers, which must
never be added to the top-level sum), `--top N`, `--json out.json`, `--include-contended`.

Exit codes: `0` fine, `1` an arm is missing/malformed/has no usable capture, `2` fewer than
two `--arm` values.

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
