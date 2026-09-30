# Grass stability and owner priority update — 30 September 2026

Owner direction: use `codex/streaming-residency`, start with low-risk work,
improve grass wind and aerial shimmer, prioritise mud puddles, and assess corpse
ragdolls. Verified baseline `ed452be8` in `.claude/worktrees/combat-audit`, aligned
with its fetched upstream. Existing untracked `.tmp-movecam/missions/` is preserved.
The root checkout is a different, older branch and is not the implementation base.

## Implemented grass slice; installed acceptance pending

- Live gust speed now divides by the shader's actual unscaled advection speed.
  The previous 6 m/s sample produced approximately 22 m/s gust travel. A zero
  live-wind sample now sets zero bend strength instead of retaining manual wind.
- Renderer-private broad/gust/flutter offsets integrate velocity once per
  terrain timestamp. Changing weather no longer multiplies velocity by elapsed
  mission age. Pause preserves phase; backwards time resets it; long gaps advance
  at most 0.25 seconds rather than jumping the field. Colour and shadow consume
  the same offsets. Public FFI size and version are unchanged.
- Blade UV derivatives fade fine texture and normal shading as width drops
  below three pixels. Photo-card contour relief fades with texture footprint.
  This preserves close detail and existing density, silhouettes and A2C coverage.
  `WGR_GRASS_FILTER_SUBPIXEL=0` restores the previous shading for a same-binary A/B.
  Native blade colour still uses the authored atlas; its normals are filtered,
  but native albedo shimmer is not claimed eliminated.
- Fixed an existing shadow uniform layout defect: `native` and `native2` were
  omitted before photo layer bounds, shifting those bounds by 32 bytes. Shadow
  blade height now also applies the native height scale used by the colour pass.

The first game/test build passed; C++ wind tests pass 17 assertions in two cases.
Rust grass tests and the complete shader composition check pass after correcting
a stale test expectation which omitted the existing native-near-blade condition.
New tests cover late weather changes, pause/rewind/long gaps and actual Naga/Rust
uniform byte offsets. The final phase/shadow source still requires the installed
game build and smoke test. These checks do not establish visual acceptance.

`scripts/Test-GrassStability.ps1` provides an isolated native-resolution profile,
fixed close/aerial cameras, six adjacent scheduled captures, before/after binary
hashes, normal-exit checks and provenance. Run it under `with-game-lock.sh`.
The screenshot scheduler can render extra frames; scheduled adjacency is not
a guarantee of adjacent rendered frames. Image differences must be inspected
over a populated grass region; stills alone do not prove absence of flicker.

## Improved execution order

1. Finish installed grass A/B and close-view regression checks; retain only
   demonstrated improvements. Extend to native photo grass and live varying wind.
2. Mud puddles: independently implement a bounded terrain-material/weather
   slice, eligible soil only, stable world-space coverage, drying and snow/shore
   exclusions. Root owns integration and serial installed runtime tests.
3. Corpse ragdolls: recheck PHY-040 against actual stock bones, matrix semantics,
   Box3D joints and both rendered/interactive corpse poses. A rendering-only
   palette override or single dynamic capsule does not satisfy this feature.
   Stock authored death animation remains the default pending a real articulated
   opt-in implementation and its collision/network/save/load gates.
4. Reassess still-open modernisation tasks against the current ledger after the
   owner-priority visual gates. This update does not enable parked streaming,
   ocean, experimental paging or previously rejected foliage defaults.

Parallel agents use separate worktrees and disjoint files. Only root deploys or
runs the game; an owner-started game is never terminated for testing.
