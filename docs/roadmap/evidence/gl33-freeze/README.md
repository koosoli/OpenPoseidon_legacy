# GL33 freeze — reference evidence (Phase 2.1)

The post-DLSS roadmap freezes GL33 as reference data before retiring it: preserve what
it rendered, not the renderer. These captures are that record's first entries.

| File | Scene | How |
|---|---|---|
| `gl33-everon-day.png` | Stock Everon, retail training mission (devtest.abel), noon | `--render gl33 --check --test-mission dev-missions\devtest.abel --test-type screenshot`, 1600×900 windowed |
| `gl33-everon-night.png` | Same, night mission (devtest-night.abel) | same, devtest-night.abel |
| `*.log` | Boot + render log of each run | `--log-file` |

**Appended 2026-08-31 — four scenes, both backends, same camera.** The gate evidence doc
(`REN-GL33-001`) marked bullet 6 PARTIAL with the note that two scenes "is not important
visual reference scenes; it is the first two entries". These are the next eight.

| Files | Scene | Content family |
|---|---|---|
| `{gl33,wgpu}-town.png/.log` | `perf_town.noe` — Nogova, roadside and hillside | terrain, roads, trees, shadows |
| `{gl33,wgpu}-water.png/.log` | `perf_water.eden` — Everon coastline | sea surface, shoreline, horizon |
| `{gl33,wgpu}-combat.png/.log` | `perf_combat.eden` | units and vehicles in a fight |
| `{gl33,wgpu}-field.png/.log` | `perf_field.eden` | open terrain and vegetation density |

Captured with, from the game directory:

```
OpenPoseidon.exe --check --render <gl33|wgpu> --window --width=1600 --height=900   --test-mission <repo>/tests/perf/missions/<mission> --test-type screenshot   --screenshot <png> --screenshot-delay 900 --log-file <log> --log-level info
```

Three things a reader should know before comparing them:

- **`--window` is not optional.** Mission mode renders fullscreen at the display
  resolution and ignores `--width`; the first pass at these was captured at 3441×1440 and
  had to be redone to match the 1600×900 the entries above use.
- **The pairs share a camera**, so they diff directly — which is what
  `scripts/compare_preview0_captures.py` and the `lint.yml` gate want. The water pair is
  the clearest illustration of what the freeze is preserving and what it is not: WGPU
  draws grass, volumetric cloud and a coloured sea with a shoreline gradient where GL33
  draws none, a flat sky and flat grey water. That is a **capability** difference, not a
  regression in either direction.
- **`farfield-bench.ps1` cannot produce these.** It refuses a GL33 arm outright —
  `gpu_timestamps_available is false ... Refusing to report that as a measurement` — which
  is right for a benchmark and wrong for an image archive. The screenshot is written
  regardless; the direct invocation above skips the argument.

**Build:** `asset-obs-001` @ `99ac0f91`, deployed as a matched pair by `scripts/Deploy.ps1`.
**Data:** retail *ARMA: Cold War Assault* + the `Install.ps1` supplement set.

- **Build:** branch `asset-obs-001`, commit `92ebd1d8` (exe and `wgpu_renderer.dll`
  deployed as a matched pair by `scripts/Deploy.ps1`; GL33 is compiled into the exe,
  the DLL is inert under `--render gl33`).
- **Data:** retail *ARMA: Cold War Assault* install + the Install.ps1 supplement set.
- **Captured:** 2026-08-30.

Scope note: GL33 never received the compat-layer parity work (WGPU is the sole target
for Arma 1/2/3, DayZ and Reforger content — see the memory/ledger), so stock CWA
content is the only family a GL33 reference is meaningful for. More scenes (towns,
water, vehicles, weather) should be appended here by the freeze work; anything present
in this directory is immutable once committed — a "fix" to GL33 that changes these
pixels is exactly what the freeze forbids.
