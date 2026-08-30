# GL33 freeze — reference evidence (Phase 2.1)

The post-DLSS roadmap freezes GL33 as reference data before retiring it: preserve what
it rendered, not the renderer. These captures are that record's first entries.

| File | Scene | How |
|---|---|---|
| `gl33-everon-day.png` | Stock Everon, retail training mission (devtest.abel), noon | `--render gl33 --check --test-mission dev-missions\devtest.abel --test-type screenshot`, 1600×900 windowed |
| `gl33-everon-night.png` | Same, night mission (devtest-night.abel) | same, devtest-night.abel |
| `*.log` | Boot + render log of each run | `--log-file` |

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
