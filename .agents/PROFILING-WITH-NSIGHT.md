# Profiling a stutter with Nsight Systems (Windows, this project)

Use this when the engine's own logs (below) cannot say WHERE a long frame went: CPU sampling shows
the call stacks of every thread during the slow frames, plus every Vulkan call and GPU workload.
First working run: 2026-10-20, Reforger Durras flight (`ns-2`). Getting it to run took a dozen
round trips with the owner; everything that went wrong is written down here so it takes one.

## Try the engine's own logs first (no admin, no Nsight)

On a Reforger flight (`scripts/reforger-pc/go.ps1`, a `job-*.txt` with `POSEIDON_FREEFLY_DRIFT=15,0,60`,
`POSEIDON_FRAME_TRACE=<prefix>`, `WGR_SLOW_BLOCK_MS=45`), the slow frames are usually explained by:

- `POSEIDON_FRAME_TRACE` CSVs: the main thread's phase per frame. On Reforger the time is in
  `swap_ms`, which is NOT just present: it is `UpdateNativeFarResidency` + `UpdateNativeClutter` +
  `NextFrame` (World.cpp, "Slow swap" log, `POSEIDON_SWAP_SLOW_MS`).
- "Wgpu slow frame" (`WGR_SLOW_BLOCK_MS`): NextFrame split into slotWait / block / trim / invalidate.
- "Native authored far: NewFromModel" and "Object stream cold model": per-model install split.
- `WGR_FRAME_LAP_MS=<ms>`: render-thread laps; `WGR_GPU_SPIKE_MS=<ms>`: a slow GPU frame's regions
  (GPU timestamps -- the worker CSV's gpu_frame_ms is wall time and overstates GPU spikes);
  `WGR_UPLOAD_REPORT_MB=<MB>`: bytes queued per frame.

## Setup (once per machine)

- **Nsight Systems 2026.5.1** is installed at `D:\Program Files\NVIDIA Corporation\Nsight Systems 2026.5.1`
  (owner's choice: D:, under `Program Files\NVIDIA Corporation`, not the drive root). The older
  2022.4.2 under `C:\Program Files` needs admin even to start and could never launch the game here;
  do not use it. Installer: `msiexec /i <msi> INSTALLDIR="D:\Program Files\NVIDIA Corporation\Nsight Systems <ver>"`
  -- run it through `Start-Process msiexec -Wait -ArgumentList ...`, because bare `msiexec` returns
  at once and the next command runs before the install exists.
- **The harness**: copy `scripts\reforger-pc\` to `%TEMP%\rfh\_OpenPoseidon-Reforger`, junction
  `%TEMP%\rfh\Remastered` -> the game folder and `bin` -> the worktree's `dist\x64-win-rwdi`
  (PowerShell `New-Item -ItemType Junction`). Keep the committed `go.ps1` and the local copy in sync.
- `llvm-symbolizer` (for the analysis): `D:\Program Files\LLVM\bin`.

## Recording (needs the owner: one admin terminal)

CPU sampling, context switches, WDDM and the one-time Vulkan-layer registration need elevation; an
agent cannot elevate. Ask the owner to open **Terminal (Admin)** and run:

    powershell -NoProfile -ExecutionPolicy Bypass -File <worktree>\scripts\reforger-pc\Run-NsysFlight.ps1

It refuses to start while any OpenPoseidon is running -- **do not launch a test game while waiting
for the owner** (a harness run of ours made the owner's run fail once). It copies the build to
`bin-nsys`, flies Durras for ~2 min with nsys recording 45..95 s, then exports (~3 min, the report
is ~350 MB and the SQLite ~3.5 GB, the converter uses ~6.5 GB RAM). Output:
`%TEMP%\rfh\_OpenPoseidon-Reforger\shots\ns-flight\nsys.nsys-rep` + `nsys.sqlite`.

Without admin, nsys 2026 still runs (`--trace=nvtx`, `NSYSTRACE=` in the job) -- use that to debug
the launch yourself before asking the owner -- but Vulkan tracing then fails ("Failed to register
Vulkan extension JSON file(s)") and there is no CPU sampling.

### Three launch traps (all fixed in go.ps1, all reported as "program not found")

1. **Junction working directory.** nsys cannot launch with `-WorkingDirectory` on a junction
   (`rfh\Remastered`): "Executable ... not found in current directory or standard search paths",
   whatever the exe path. go.ps1 passes the junction's target.
2. **Literal quotes.** nsys takes `"C:\...exe"` quotes as part of the program name; go.ps1 quotes
   the exe only when the path has a space.
3. The exe behind the `bin` junction: Run-NsysFlight.ps1 uses a plain copy (`bin-nsys`) anyway.

## Analysis

    python -I scripts\reforger-pc\nsys_long_frames.py <...\nsys.sqlite> --bin <...\bin-nsys> --skip-s 56

Long frames = gaps > 50 ms between `vkQueuePresentKHR`. Prints the main thread's and `wgr-render`'s
inclusive/leaf functions in those frames and the costliest Vulkan calls. `--skip-s` drops the
loading period (the flight starts ~55 s into the capture).

**Symbols.** nsys leaves our frames as raw addresses and the export has no module-load table. The
script recovers the load addresses itself (outermost exe frame = `__scrt_common_main_seh`, first DLL
frame above an exe frame = an `ffi::wgr_*` export; every 64 KB-aligned base is tried) and needs the
exact traced binaries + PDBs -- `bin-nsys`, not a rebuilt dist. EXE bases change every launch, so
reading them from a fresh process does not work. Use llvm-symbolizer's JSON output: the text
format prints a varying number of lines for unknown addresses and silently misaligns a batch.

Raw tables if you need more: `COMPOSITE_EVENTS` (sample time/thread) + `SAMPLING_CALLCHAINS`
(frames by `id`, `stackDepth` 0 = leaf, `module`), `VULKAN_API`, `VULKAN_WORKLOAD`, `SCHED_EVENTS`,
`MEMORY_TRANSFER_EVENTS` (WDDM paging), names in `StringIds`. Never query per sample -- 59 M frame
rows; join through a temp table.

## What the first trace showed (ns-2, build 81493804a, 2026-10-20)

64 frames > 50 ms (5.2 s). Main thread in those frames: 36% `Landscape::UpdateNativeFarResidency`
(19% `EngineWgpu::RegisterGpuModel`, 10% `ShapeBank::NewFromModel`, 10% `SceneFarObjectCreated`,
9% the residency loop's own time). wgr-render: 57% `render_frame`, of it 24% wgpu
`command_encoder_finish` (encode bookkeeping), 19% upscaler evaluate, 12% submit. Whole capture:
534k `vkBeginCommandBuffer` (~270 per frame, 2.2 s), 1.2 M `vkSetDebugUtilsObjectNameEXT`,
249k `vkCreateBuffer`.
