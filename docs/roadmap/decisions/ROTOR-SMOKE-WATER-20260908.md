# Rotor, smoke, lamps and boat follow-up (2026-09-08)

Worktree: `.claude/worktrees/asset-obs`, branch `asset-obs-001`.
Base: `725ccc0c`. Foreign `Cargo.lock` is excluded and unchanged.

## Changes and evidence

- Wgpu helicopter CPU poses receive separate reusable vertex buffers per queued
  mesh draw. Previously later instances could overwrite the same pool range before
  earlier draws executed. `WGR_HELI_POSE_SNAPSHOTS=0` restores the old path for A/B.
  The copy is scoped to its source Shape, not unrelated subsequent sections.
- Helicopter LODs without a blurred rotor selection retain the rotating blade
  selection. `POSEIDON_ROTOR_DIAG=1` reports instance position, RPM and LOD.
  Stock UH60MG visual LODs already have blur selections, so this fallback alone
  does not explain every reported distant-rotor failure.
- Zeus street-lamp tools expose authored/override colour and intensity 0..8.
  Generation invalidation uses the existing StreetLamp::CreateLight rebuild path.
  Built successfully; interactive control operation is not yet visually verified.
- Follow-up: the same section now offers optional colour-temperature mode,
  1000..12000 K, separate from custom RGB. Temperature/mode changes increment
  the same generation and existing lamps resolve the selected colour when rebuilt.
  Default RGB and intensity are unchanged; turning off the colour override restores
  authored colours. Pool size, spill strength/reach/ambient and glow remain available.
  Temperature is an approximate display-RGB fit, not a spectral lamp simulation:
  [formula reference](https://tannerhelland.com/2012/09/18/convert-temperature-rgb-algorithm-code.html).
  Game/tests build passed; targeted light/decal/basis tests passed 359 assertions
  in 4 cases, including temperature endpoints, non-finite input and unchanged mode
  default. Installed as paired EXE/DLL 63264739 at 22:36 after the game lock became
  free. The 20-second installed night capture temperature-build-smoke completed
  normally, without a panic/renderer validation error. Interactive Kelvin/RGB
  slider operation still needs visual verification; the capture uses default RGB.
- Smoke self-shadow density is summed once per occupied cell, then sampled without
  repeatedly walking every neighbour. Unit tests compare to the previous sum.
  Empty-space sphere sweeps now reject cells with no overlapping object bounds
  before the seven exact rays. Candidate collisions retain the previous narrow phase.
- Small planing boats cap the extra wave-induced draft used by horizontal drag at
  0.25 m above mean draft. Buoyancy, vertical damping and large ships are unchanged.
  `POSEIDON_BOAT_WAVE_DRAG=0` provides the old response; `POSEIDON_BOAT_DIAG=1`
  logs speed, position and pilot commands.
- High-wave foam uses band-limited, wind-oriented non-periodic coverage modulation.
  Shore/river foam is unchanged. Visual taste and remaining seams are still open.

## Installed-game checks

All deploys and launches used `scripts/with-game-lock.sh` and matched
OpenPoseidon.exe/wgpu_renderer.dll, never the retail executable.

Private evidence is in `.tmp-rotor-smoke/` and `.tmp-nogova/` (not source assets).
Nogova, RTX 3070 Vulkan, 1280x720. Dirty build provenance is intentional and recorded
in deployment stamps rather than attributed to a clean commit.

- Smoke control/optimized 8x tests: same fixture and 18 m/s wind, 30 s runs.
  Samples with >=3800 particles averaged 10775 us (9 samples) versus 7860 us
  (8 samples), about 27% lower CPU simulation time. Both had density aggregation;
  this A/B measures the broad phase alone. Comparable plume positions retained
  11481 versus 11464 collision hits. This is not a full performance benchmark or
  proof that every downwash case is solved. Sweep/ray diagnostic counters are
  cumulative, not per-frame; the current log label is `rays_counter`.
- BoatW crewed fixture, wave amplitude 3, wind 18: at simulation t=19.1 s the
  modified response reached 11.952 m/s versus 4.373 m/s in the legacy control,
  with near-full thrust and no braking. Buoyancy remained active in both runs.
- Rotor control/snapshot captures show the parked UH60MG retaining its full
  stopped rotor with snapshots enabled. Two crewed helicopters report RPM 1.0,
  the parked one 0.0. However the flying helicopters are absent from these
  captures despite plausible logged positions. Their visibility and the original
  distant-blade report remain OPEN, not accepted as fixed.
- Final 19:32 deploy/capture includes the source-Shape guard and XYZ rotor log.
  No missing road segments in `rotor-final.png`; the earlier isolated screenshot
  difference was not established as a regression.
- C++: 117 assertions / 19 cases passed for density, ship, helicopter,
  ViewDistance and GameSettings. Rust: both shader-validation tests passed.
  Game and unit-test targets build successfully.

## New authorised scope, not yet fixed

Cloud trailing with DLSS/godrays (cause needs A/B); player head shadow; human
shadows on water; campaign boat entry/exit drowning; dead infantry floating;
selective upstream improvements. Upstream/main fetched at `3541affa`.
No blind merge or GL33-only port is authorised by this evidence record.

Initial infantry inspection: SoldierOldSim.cpp grants spring buoyancy only to
the player. AI still takes depth-based drowning damage; corpse collision records
water depth but only solid ground supports the body. Treat these as separate
simulation issues, not as proof of the unconfirmed mission-specific boarding bug.
