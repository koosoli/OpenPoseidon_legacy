# Where the GPU frame actually goes (2026-08-24)

Everything here was measured on `perf_abel`, wgpu backend, exe and DLL from the same commit,
256 settled frames per capture. **Resolution is stated on every figure** — see the trap list
below for why that sentence exists.

---

## The one-line summary

The frame is **GPU-bound with a ~15 ms resolution-independent floor**, the largest items in it
are **clouds** and **grass**, and "shadows" are not shadows — they are grass being drawn into
cascades that cannot receive it. Fixing that last one is the only optimisation this session:
**−1.8 ms, ~8%, no visual change.**

---

## The frame model

Three resolutions, 2 repeats each, all four regions instrumented (`abb9f22e`):

    GPU frame = 15.07 ms + 3.53 ms/Mpix        fit error <= 0.12 ms across a 4x range

    1280x720    18.3 ms   54.6 fps    82% of the frame is the fixed floor
    1920x1080   22.4 ms   44.7 fps    67%
    2560x1440   28.1 ms   35.6 fps    54%

**There is a hard ~66 fps ceiling no matter how low the resolution goes**, on a scene with
22.6k triangles and 36 colour draws. Dropping resolution buys far less than anyone expects.
Any future perf work that ignores the floor is optimising the smaller half of the frame.

---

## Full attribution at 1920x1080

Everything the renderer records is now inside a bracket — 44 regions reporting, 30 outermost.

| item | ms | note |
|---|---|---|
| colour sub-pass (container) | 9.69 | clouds 5.41, grass ~2.2, water, transparents |
| shadow cascades (4) | 4.26 | **almost entirely grass** — see below |
| *unattributed* | 2.24 | **between** passes, not in any of them |
| water FFT / spectrum / foam | ~2.0 | |
| depth+normal prepass | 1.71 | pays for itself; also required by AO |
| GTAO | 1.06 | |
| culling (main + shadow + Hi-Z) | 0.64 | cheap |
| interior sky visibility | 0.57 | runs on an open field, but see below — **not reclaimable** |
| post chain (all of it) | 0.63 | god rays .307, HDR resolve .101, bloom .101, tonemap .062, exposure .061, UI .002 |

### The CPU is not the bottleneck and has not been for a while

CPU frame 24.64 ms of which `swap` — blocking on the GPU — is **95.2%**. Real CPU work is
**1.18 ms**. GPU-driven culling, grass placement, water FFT and vegetation conform are all on
the GPU already. The 217 CPU-replayed direct draws cost **0.000 ms**. There is nothing left
worth moving off the CPU.

---

## Findings, in order of size

### 1. Clouds are the largest single item — 8.9 ms, 26% of the frame

The 3D colour sub-pass reported 12.8 ms while every leaf inside it summed to 3.8. The missing
9 ms was the volumetric cloud march, which had **no region, no quality tier and no env var**.
Half-res march (960x540 at 1080p), `CLOUD_STEPS` 128 x `CLOUD_LIGHT_STEPS` 4 = up to 640
density samples per pixel.

`WGR_CLOUD_STEPS` (`296ead47`) is the knob. Default unchanged at 128 — the owner chose to keep
it. Ladder, region-level: 128 -> 7.90, 64 -> 5.96, 32 -> 4.39 ms. **Halving does not halve**:
the step count is adaptive from the ray's path length and the cap only binds on long/grazing
rays. Visually, 64 shows speckle in cloud bodies and 32 is badly grainy.

### 2. Grass is ~7 ms across three passes, and had no player control

Shipped at density 1.0 / 41 m ring ("ultra dense is the production default") reachable only
through undocumented env vars. A Grass Detail row now exists (`15cf592b`), Ultra reproducing
today's values exactly. Ladder at 3441x1440: Ultra 33.17, High 30.70, Medium 27.42, Low 24.90,
Off 21.92 ms. What changes down the ladder is the **radius**, which leaves a visible bald ring
of ground — High is nearly free of that artifact, Medium is where it becomes obvious.

### 3. "Shadows" are grass — and this was the free win

Four cascades cost **4.443 ms with grass and 0.121 ms without**. Objects are 0.12 of it.

What gave it away is what the cost does *not* depend on:

| variable | change | effect |
|---|---|---|
| shadow map 2048² -> 512² | 16x fewer texels | 3.987 -> 3.741 ms — nothing |
| casters 1,426 -> 89,085 tris | 62x | 0.934 -> 0.943 ms — nothing |
| cascade count 4/3/2/1 | — | 4.012 / 2.852 / 1.926 / 1.197 — linear |

Flat in raster resolution, flat in triangles, linear in pass count = **vertex-bound
instancing**. `Grass::draw_shadow` submits the near-ring blades into whatever pass it is
handed, so they are transformed once per cascade and then clipped away by the cascades whose
depth range the ring never reaches. Splits are 0.1 / 20.7 / 45.1 / 100.2 / 400 m for a 41 m
ring — cascades 2 and 3 could not receive a single grass fragment.

Fixed in `3d53fb66` with `frame.shadow.grass_cascade_mask`, derived in C++ from the split
distances and the grass reach, occupying the old pad word so the ABI is unchanged.

**Measured A/B on one binary, 3 repeats, medians (`156e8ef8`):**

    grass in all 4 cascades    frame 25.233 ms    shadow 4.028
    derived mask (0x3)         frame 21.469       shadow 2.192

Quote **−1.836 ms** (the shadow region — it reproduces across independent pairs). The frame
median moved 3.764 ms but the old arm spread 23.3–26.1 against the new arm's 20.9–21.7, so the
frame figure carries drift the region figure does not.

**Why it is genuinely lossless.** Cutting at the ring radius alone would have been wrong: a
blade at the edge casts a shadow that *lands* further out, with a horizontal run of
`height / tan(elevation)` — ~11x blade height at 5°. That regression is invisible at noon and
obvious at dusk, the worst possible failure mode for anything verified by screenshot. The reach
carries a sun-angle margin and it demonstrably engages:

    high sun   reach 41.4 m   mask 0x3   cascades 0,1
    +5 h       reach 44.0 m   mask 0x3
    +7 h       reach 48.0 m   mask 0x7   cascade 2 returns on its own

`WGR_GRASS_SHADOW_CASCADES=4` restores the old behaviour for A/B.

### 4. ~10% of the frame is *between* passes

With every named pass bracketed, 2.24 ms at 1080p is in no region: ~1.6 ms fixed +
~0.5 ms/Mpix. Roughly 30 passes at ~53 µs of gap each — render-pass setup/teardown, layout
transitions, pipeline drain/fill between dependent passes. **Pass count is a first-class cost
here**, not just pass content.

---

## Traps — read this section before measuring anything

Every one of these produced a wrong answer that looked right.

1. **The harness benchmarked a different binary than Deploy wrote.** `farfield-bench.ps1`
   launched `ColdWarAssault.exe`; `Deploy.ps1` wrote `PoseidonGame.exe`. Fixed by renaming to
   one name, `OpenPoseidon.exe` (`7eb66103`). Benchmarks now refuse an unstamped install and
   record the deployed commit in `run-NN.meta.json`.

2. **`-Width/-Height` was a request mission mode declined.** `--window` was only added in world
   mode, so mission runs went fullscreen at the display's native resolution. Half a day of
   figures were quoted as 1920x1080 while every capture was 3441x1440 — 2.4x the pixels. Same
   scene, cloud steps 128: 3441x1440 -> 36.33 ms, 1920x1080 -> 23.08. Fixed (`f253e960`):
   `-Windowed`, and the capture resolution is read back from the PNG and reported as a failure
   on mismatch. **Read `capture_width` before quoting any ms figure.**

3. **`-ScreenshotDelay` defaulted to 10 frames against a 256-frame ring**, so every mission-mode
   capture described mission *load*: 11 frames, 1.1 fps, `land:gnd` averaging 530 ms and peaking
   at 5.8 s. The JSON looks entirely normal; `sampled_frames` was the only tell. Default is now
   900 and under 200 is reported as failed.

4. **`WGR_PREPASS=0` disables GTAO too.** Reading it as a prepass isolation says the prepass is a
   net loss. It is not: with AO off in both arms, prepass ON 20.958 vs OFF 20.907 — zero. It
   costs ~1.88 ms and returns ~1.62 ms of colour-pass overdraw, and it produces the normal
   buffer AO reads.

5. **`SetMsaaSamples`, `SetRenderScale` and `SetAlphaToCoverage` have no `EngineWgpu` override.**
   They hit an empty base virtual, so on wgpu those config fields do nothing. MSAA is fixed at
   renderer creation from `WGR_MSAA`. A config-driven MSAA ladder (off/2x/4x/8x ->
   33.31/34.61/35.68/35.15) looked monotonic and was pure drift; the log said `MSAA enabled: 4x`
   throughout. **Anti-aliasing and Supersampling are inert rows on wgpu.** 8x also silently
   falls back to 4x.

6. **A pixel diff cannot validate a render change here.** Two runs of the *same* config differ
   in 69–78% of pixels (>24/255 in 19–28%) because wind and cloud drift are not frame
   deterministic. Always run the same-config control first; prefer a geometric argument.

7. **The 1 ms log-timestamp trace cannot resolve frame alternation.** A steady 26.5 ms frame
   reads as 26,27,26,27 forever. `triPerfSeries` (`3d2c8966`) dumps FrameProfiler's float ring
   instead. Real result: an oscillation exists (flip 63.6%, lag-1 r −0.409) but is **not
   parity-locked** (even/odd means differ by 0.04 ms) and is ~1.6 ms, ~6% — a free-running beat
   consistent with a CPU blocking on a GPU, not "extra work every second frame".

8. **In-pass timer brackets must be gated on `time_segment`.** Ungated, the last plan segment —
   the post-Resolve UI one — wins the single query slot, and `ops pre-water` reported 0.000 ms
   for a replay containing 3.8 ms of grass.

9. **Within-arm spread reached 1.11 ms at 1080p.** Treat any single-repeat difference under
   ~1 ms as unresolved.

---

## Not reproduced

The Discord report of "higher render effort almost every second frame" does **not** reproduce
here. Frame health at 1080p, stock Abel: static median 28.0 / p99 32.0 / worst 49.0 ms; moving
median 27.0 / p99 36.0 / worst 38.0. Nothing over 100 ms, moving no worse than static. A 2x
alternation would be enormously outside that band. Closing that report needs a `triPerfSeries`
trace from the reporter's own machine.

---

## Open, in priority order

1. **~1.6 ms of inter-pass overhead** across ~30 passes. Structural — merging compute chains.
2. **The 15.07 ms floor** is the real ceiling. Shadows were its largest item and are now halved;
   next largest are water FFT (~1.1), culling (0.64), interior sky (0.57).
3. ~~Interior sky visibility runs outdoors~~ — **measured and rejected.** It looked like 0.57 ms
   of pure waste on a field with no buildings. Three repeats each, 1920x1080: `WGR_SKY_VIS=0`
   medians 21.868 ms against 21.346 with it ON, i.e. turning it off is not faster. The regions
   report the time but it is not additive to the frame. Not a lever; do not spend the day on it
   as I nearly did.
4. **Anti-aliasing and Supersampling rows are inert on wgpu** (trap 5). They now persist a value
   nothing reads.
5. **Never visually confirmed**: the Grass Detail row rendering in Options and surviving a
   restart. Everything measured here drove `graphics.cfg` directly.
