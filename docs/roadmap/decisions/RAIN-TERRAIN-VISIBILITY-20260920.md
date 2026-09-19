# Heavy-rain terrain visibility repair

## Reproduction and cause

With full weather rain and weather fog zero, free flight on Nogova showed distant trees and buildings suspended above missing, rectangular terrain regions. The installed pre-fix `c9006e58` build reproduced this on the three-position route in `scripts/Test-RainTerrainVisibility.ps1`; `build/rain-terrain/before-heavy-rain-20260920-000815/moved.png` shows the fault.

`Weather::Simulate` reduces tactical visibility to roughly 350m at maximum rain, further reduced by daylight. `Scene::ResetFog` used that same number for terrain bounds. WGPU intentionally disables distance fog when weather fog is zero, while the object draw distance retains a floor at the selected object distance. The result was unfogged objects beyond a weather-shortened terrain rectangle, not delayed disk streaming.

## Fix

Committed/installed code: `52ee1cb7`, branch `codex/ocean-release`, pushed to the safety remote. In WGPU with zero weather fog, the display range now retains the selected view distance (minus the existing 20m margin). Terrain bounds, object draw distance and the aerial-range base use that display range. Shadow fog, tactical fog, tactical visibility and `ENGINE_CONFIG.horizontZ` retain their previous weather policy. Explicit weather fog and GL33 retain their prior range policy. This also avoids the same render mismatch from the night-time tactical reduction when weather fog is off.

No shader, streaming-cache size or per-frame terrain mesh budget was added. The renderer restores the ordinary selected-distance geometry; drawing terrain that was previously missing necessarily costs more than the broken rain view. The old ocean remains the default.

## Installed verification

`Build.ps1 -Target PoseidonGame` passed (`build/rain-terrain-build.log`). Matching executable and WGPU DLL were deployed under the game lock. The harness uses the actual developer free-flight camera and production forced-rain path, a private profile, 1280x720, MSAA4 and 2000m selected view distance.

- Before-fix rain: missing distant terrain visually reproduced.
- Fixed rain: `build/rain-terrain/fixed-heavy-rain-20260920-001131/moved.png` shows continuous terrain under the same distant assets. All three moving-camera probes report terrain/object/base range **1980m**, rain **0.999999**, fog **0**, and tactical visibility **262.5m**.
- Dry control: the same three probes report terrain/object/base range **1980m**, rain/fog **0**, and tactical visibility **2000m**.
- Explicit fog 0.5 still reduces the base range: **137.8m** in rain and **472.5m** dry. These are the unchanged weather-limited paths, with WGPU fog enabled.
- Both fixed runs and the baseline exited cleanly without renderer validation errors. Two screenshots per run were taken only after the twelve-second performance interval. No ongoing game session was interrupted.

Median whole GPU frame time (six logged samples per interval): broken rain **12.445ms**, fixed rain **14.296ms**, fixed dry **13.4905ms**. Terrain GPU time: **0.989 / 1.7505 / 1.648ms**, respectively. No logged slow frames in any measured interval. The repair restores missing geometry, so this is not a claim of unchanged cost relative to the broken case. Fixed rain uses the same terrain reach as clear weather; clouds and rain add their own cost. These short checks do not establish performance on other GPUs or at maximum view distance/particle counts.

Raw metrics, visibility probes and screenshot SHA-256 hashes: `docs/roadmap/evidence/rain-terrain-20260920.json`. Owner confirmation on the original map/settings remains welcome; formal owner visual acceptance is not claimed.
