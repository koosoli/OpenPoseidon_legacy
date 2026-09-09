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

- fa7a7af2: corrected bicubic filter built, numerical reference passed, installed
  low-altitude ocean capture exits normally. Grid-like marks remain in that
  particular view, so this correctness fix is not full acceptance of the owner's
  near-water screenshot. Capture pair `.tmp-bulb/sep9-water-close-before/after.png`.
- c625e1ea: reversible experimental Showcase crater at 9700/3600, 24m radius,
  8m depth on Desert's 6.25m terrain grid (45 changed samples). CPU SurfaceY,
  GPU heightmap/quadtree and Box3D heightfield all update; water bounds refresh
  without restarting FFT. GPU material/grass atlases are not reuploaded.
  Replacement physics terrain is constructed before destroying the old one;
  dynamic bodies wake after replacement. Synthetic resting-body test confirms
  settling into the excavated bowl with the original body handle intact.
  62 cases / 15989 assertions passed. Installed harness exits 0, rejects a
  duplicate crater and restores it. Log centre 29.835 -> 21.835 -> 29.835m;
  `terrain-before/crater/restored.png` images show the geometry change.
  Limitations: explicit LAB actions only, full heightfield rebuild pause,
  single-player unsaved prototype, no automatic grenade damage, no tunnels,
  not a general solution for navigation or objects built over excavated ground.

- 8a4161a8 / 9bded011: automatic legacy far albedo built from decoded source
  tiles, 1024-square with mipmaps, wide cell-edge blending, transition 800-2400m.
  No baked light/snow, existing authored satellite/Enfusion materials unchanged.
  `WGR_OFP_SATMAP=0` disables it for A/B. Installed Everon: 241 source tiles,
  111ms startup bake; Nogova: 302 tiles, 485ms, ~5.3MiB texture including mips.
  Initial 256-tile cap excluded Nogova and was corrected to a bounded 1024;
  missing/undecodable source tiles abort the bake rather than inserting white.
  Installed capture exits normally and is smoother at range, but cannot invent
  photographed terrain detail absent from OFP source art. 11 cases / 13529
  assertions passed for satmap/crater/CDLOD before the follow-up.
- 9bded011: Everon dirt-road sheen reproduced at owner's exact camera pose.
  `WGR_GLASS_SHEEN=0` ablation removes it; geometry-aware glass classification
  now excludes OnSurface decals while retaining actual glazing's alpha rule.
  Installed default-on-glass capture confirms the road no longer has blue sheen.
  99 cases / 1140 assertions passed for Material/satmap/crater/LightVisibility.
- Room investigation: explicit vectorUp reads [0,0,1] for both roof and back wall;
  common ASL elevations are correct. Uprighting makes no difference. Bright
  edge bands therefore are not explained by slope alignment; investigate
  shadow leakage/rendered geometry before treating the room as finished.
- 8d2fc2f1: ocean axial-far vs radial-corner correction built, 18 cases / 13557
  assertions passed, pushed both safety refs and installed. Same 2048x857 camera
  at 80000/7000/40000, heading 0, pitch -80 shows increased coverage but still
  a stair-step ocean edge. This is partial, not acceptance. CPU water still clips
  at the land far plane despite the GPU using infinite-far projection. Follow-up
  removes that water-only clip and derives reach from sea/frustum intersections,
  bounded by the existing last ocean LOD range.
- afee9c8f: follow-up built, 10 CDLOD cases / 13494 assertions passed, pushed
  both safety refs and installed. Same camera now has ocean over the entire
  image, no stair-step edge. Draw timings in this view increase from roughly
  2.8-3.9ms to 4.1-4.9ms because formerly missing pixels are now shaded; FFT
  allocations/resolution unchanged. Not an infinite-orbital-space solution.
- Smoke follow-up in progress: surface shadow sampling incorrectly returned
  fully lit when cloud strength was zero, ignoring the independent smoke channel.
  Removed that gate. Growing-puff opacity is shared by sprites, self-shadow,
  ground shadows and volume injection; previously injection omitted expansion
  dilution and was multiplied by the ground-shadow control. Wall clipping no
  longer increases density. Initial 11 smoke cases / 409 assertions passed;
  final build, shader contract and installed comparison pending. No wind,
  downwash, collision or emission-lifetime changes in this patch.

Other queue entries remain open. In particular local lights currently contribute
only diffuse/ambient in `lighting.wgsl`; the glossy material's direct specular
term is sun-only in `shading.wgsl`. Night GLOSS demo is not considered fixed.

- c59deee1 smoke follow-up built, pushed both safety refs and installed. Rust
  cloud/smoke independence contract passed; final density/contact tests 3 cases /
  43 assertions passed. Installed Desert smoke capture exits 0 and still shows
  drifting smoke and its ground shadow. This fixture already had shadows before;
  it does not establish acceptance for every reported missing-shadow situation
  or zero cloud strength through the interactive settings.
- Local-light specular implementation pending acceptance: separate raw RGB
  appended to WgrDraw3D (272 -> 288 bytes; both size assertions and automatic
  layout hash handshake), GPU Material 112 -> 128 bytes, retained/per-draw parity.
  Sun highlight unchanged. Local highlight reuses existing attenuation and
  spot/cube occlusion, no duplicate shadow taps; diffuse-only callers retain
  their wrapper. Build passed before adding the source contract; committed
  build, shader contract and installed GLOSS comparison still pending.
- 74369a27: matched EXE/DLL built, pushed and installed; local-specular source
  contract passed, full Showcase harness exits normally. Room GLOSS panel now
  has visible warm highlights absent in the previous image. Bright room edge
  bands remain. Exact Everon road camera still has no blue glass sheen.
- Snowflake legibility follow-up: diagnostic reports roughly 500 drawn flakes
  but the captured image shows virtually none. Replace additive Flare0 sprites
  with a 32-square mipmapped white/soft-alpha particle texture, same count and
  geometry. Texture releases on world/particle reset. Build/test and installed
  comparison pending; accumulation roof exclusion remains a separate open item.
- 173733a4: built, 32 Rain/Snow/Weather cases / 38063 assertions passed,
  pushed and installed. Flock visibility now confirmed, but the fixed-exposure
  scene shows dark particles: legacy RGBA8 sprites cannot carry the HDR sky's
  higher radiance. Follow-up adds an explicit precipitation radiance gain
  (negative 2D fog lane, ABI version 10 on both sides; normal sprites unchanged)
  and hemisphere-averaged daylight gain, plus texture release at world cleanup.
  Build and installed image acceptance pending. This is approximate flake
  illumination, not per-particle shadow/local-light evaluation.
