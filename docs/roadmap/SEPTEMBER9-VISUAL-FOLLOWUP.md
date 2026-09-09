# September 9 visual follow-up

Owner authorised implementation after backup `codex/backup-asset-obs-20260909-bc766e19`.
Worktree asset-obs only; foreign Cargo.lock excluded. Screenshots 070351, 070456,
072225 and 073829 are reproduction evidence, not completion evidence.

## Queue and acceptance

- Experimental destructive terrain: real height/collision changes with bounded
  invalidation, not a decal or snow-only depression. Do not claim tunnels yet.
- Showcase: verify normal/gloss examples (owner wrote glass), add actual glass
  if appropriate; large shootable block castle/walls; enterable dark rotating-light room.
- Snow: visible flakes, roof exclusion for accumulation, ground variation and
  actual geometric tracks/depressions, not solely albedo/normal shading.
- OFP satellite far appearance: prototype now explicitly authorised for quality.
- Ocean: camera-independent continuous coverage and no brown offshore tiles.
- Water specular: investigate blocky near reflection with bounded extra cost.
- Wave foam intensity: independent gain and profile persistence. In progress.
- Lighting reset: dedicated button plus master reset; advertise actual coverage.
- Lamp and vehicle bulb glow: replace visibly dated sprites, retain occlusion.
- Local shadow source size/penumbra and abrupt loss while light still reaches.
- Everon dirt-road glassy sheen at player 5083.78/3988.61/16.33,
  camera 5083.67/3988.25/17.63, heading 145.4, elevation -18.5.
- Distant freefly lighting with live player: reproduce two-camera regression.
- Picture Mode: max blur 4, optional player follow and click focus. In progress.
- Smoke: dispersal/dilution/despawn and ground shadow regression.
- Godrays on distant foliage/forest LODs: image 073829 contrast mismatch.

## Initial investigation

Installed baseline a4d09dbe. Private Nogova aerial at 30 km altitude shows water
throughout, not the owner's tile failure; 120 km exceeds useful camera/atmosphere
coverage and produces a different black-background failure. Neither is acceptance
of the reported view. Raw `.tmp-bulb/sep9-water-*-before` files retained.

Found: whitecap shader gain is multiplied by shoreline gain despite UI promising
independent controls, and whitecap fields are absent from water-look persistence.
Master reset already includes sky/water/roads but does not list them; lamp/glow/
shadow settings are actually missing. Fixes and tests in progress, not yet deployed.

## Implemented checkpoints (not overall completion)

- 684b2871: independent wave-foam gain, persisted whitecap settings; dedicated
  lighting reset and master reset coverage; Picture Mode max blur 4, optional
  player-follow and free-fly click focus. 58 C++ cases / 4759 assertions and
  whitecap Rust contract passed. Installed DOF lifecycle capture exits normally;
  interactive focus and foam A/B acceptance still pending.
- ac5eaaa9 / bcaec198: snowflake billboard length no longer collapses when viewed
  along its velocity. Optional close-up snow geometry refines selected terrain
  patches to <=0.125m samples within an 8m camera window, plus accumulation drift
  variation. Collision remains bare ground; accumulation roof mask still open.
- 4d14628d / 1714d132: large castle preset (485 dynamic blocks after adding a
  supported lintel), ~32m span, 8m walls, 11m corner columns; moving room light,
  material-room action entries. Build and 66 cases / 16007 assertions passed
  before lintel follow-up; follow-up has 492 assertions / 2 cases passed.
  Installed and pushed both safety refs; original backup ref untouched.
  `scripts/Test-Showcase.ps1` exits normally with castle and light-on/off captures.
  Images confirm standing gate and daylight room illumination toggling. Room
  still has visible edge gaps; normal/gloss contrast and actual castle impact
  remain acceptance work, not closed merely because the harness returned OK.
- Investigation found incorrect bicubic FFT dynamics reconstruction: non-adjacent
  pair sums, cross-axis denominator and reversed blend weights. Corrected using
  the same four texture fetches; numerical reference test added. Not deployed yet.

All other queue entries above remain open. No destructible-terrain or automatic
satmap implementation is claimed by these checkpoints.
