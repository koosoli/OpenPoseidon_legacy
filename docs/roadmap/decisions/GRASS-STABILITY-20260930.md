# Grass stability and owner priority update — 30 September 2026

Owner direction: use `codex/streaming-residency`, start with low-risk work,
improve grass wind and aerial shimmer, prioritise mud puddles, and assess corpse
ragdolls. Verified baseline `ed452be8` in `.claude/worktrees/combat-audit`, aligned
with its fetched upstream. Existing untracked `.tmp-movecam/missions/` is preserved.
The root checkout is a different, older branch and is not the implementation base.

## Implemented grass slice; visual acceptance still open

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

The game/test build passed; C++ wind tests pass 17 assertions in two cases.
All 25 Rust grass tests and the complete shader composition check pass after correcting
a stale test expectation which omitted the existing native-near-blade condition.
New tests cover late weather changes, pause/rewind/long gaps and actual Naga/Rust
uniform byte offsets. Installed matched `c3f4b9d3` passes normal-exit captures with
populated grass at close/aerial stock scenes and Oliver's exact OFP Everon camera.
Both default and filter-off 1x/4x GPU pipelines validate. These checks do not
establish visual acceptance.

`scripts/Test-GrassStability.ps1` provides an isolated native-resolution profile,
fixed close/aerial cameras, six adjacent scheduled captures, before/after binary
hashes, normal-exit checks and provenance. Run it under `with-game-lock.sh`.
The screenshot scheduler can render extra frames; scheduled adjacency is not
a guarantee of adjacent rendered frames. Image differences must be inspected
over a populated grass region; stills alone do not prove absence of flicker.
`python scripts/Score-GrassStability.py` reproduces the fixed-ROI RGB diagnostics
and excludes failed, empty-grass, incomplete or incorrectly sized captures.

### First installed comparison and owner reproduction

Installed evidence is in ignored `build/grass-stability/`, with hashes, camera,
profile, screenshots, metrics and normal-exit results per run. Corrected stock
aerial runs `094409`/`094512` and repeat `094850`/`094832` have 89,184 near and
2,700 mid instances. Close runs `094553`/`094709` retain visible blades. Initial
`first-*` subterranean captures have zero grass and are excluded from acceptance.

Oliver's OFP Everon pose is X/Z/altitude `5018.51 4087.66 117.58`, azimuth `220.8`,
elevation `-86.0`. Runs `owner-OwnerEveron-0-20260930-095543` and
`owner-OwnerEveron-1-20260930-100039` render 112,164 near/18,466 mid instances,
photo cards **off**, native resolution 1280x720, MSAA 4, fixed noon lighting.
The bright aerial speckles remain obvious in both. The first normal/texture
filter therefore does **not** close Oliver's reported disco-ball defect.

Offline RGB analysis uses a fixed grass ROI `[400,250,900,600]` over five
scheduled-image deltas. Pixels with any channel delta >48 fall from 1.4128% to
1.0578%; mean luma changes from 117.726 to 115.195. This includes coherent wind
motion and screenshot timing, and is a diagnostic rather than a per-frame
flicker measurement. The root/tip procedural ramp remains unfiltered in this
first build; a follow-up must address its unresolved colour variation and
recheck the exact owner pose without deleting geometry or changing density.

### Procedural ramp follow-up

`239cd386` carries the blade's actual taper to the fragment stage and blends
unresolved procedural shading to the analytic area mean of colour/root/gust,
including the correlated dry-tip colour and height AO. Close resolved shading,
native authored albedo, world-scale patch tint, density, silhouettes and coverage
are retained. The approximation integrates the ideal continuous taper rather
than every projected curved triangle and does not integrate all lighting jointly.
All 27 grass tests pass, including GPU execution of production helpers compared
with independent 65,536-sample numerical integration across 16 cases and real
1x/4x pipelines. This is a mean-preservation math test, not an image-brightness claim.

Installed matched source `090a513e` (stamp `+dirty`: only status documents and
Cargo's unrelated generated lockfile difference, no uncommitted rendering code)
passes owner-pose runs `averaged-OwnerEveron-1-20260930-101857` and same-build
`averaged-control-OwnerEveron-0-20260930-101953`. Bright white speckles are visibly
suppressed. Identical grass counts are retained. In the same ROI, >48-channel
deltas fall from 1.3937% to 0.0795% (about 94%); mean delta falls 5.478 to 2.816.
Aerial mean luma changes 117.686 to 105.845 as unresolved bright tips are averaged.
These are scheduled-image diagnostics including wind/capture timing, not a proof
that every rendered frame is shimmer-free.
Repeat `averaged-repeat-OwnerEveron-1-20260930-103709` retains the improvement:
>48 deltas 0.0770%, mean delta 2.786 and mean luma 105.839.

Close captures `averaged-Close-1-20260930-102050` and
`averaged-control-Close-0-20260930-102340` retain visible individual blades;
mean ROI luma is 115.530 versus 114.309. Large scheduled-image changes drop from
6.6687% to 2.4760%. A single GPU snapshot shows grass colour 3.93 ms filtered
versus 3.76 ms control, with prepass 2.49 versus 1.78 ms; these short single cuts
have timing variation and are not a settled median benchmark or a speedup claim.
Oliver's preferred near/distant appearance and native atlas temporal acceptance
remain visual gates. Rain/puddle source changes in this pair are dry in the stock
grass comparison and do not redefine grass admission.

### Settled repeated timing and owner delegation

Oliver deferred appearance tuning on September 30 because local pictures do not
load in his mobile app and asked us to use our own judgement and keep building.
Owner review is therefore a later tuning step, not a blocker for this bounded
implementation. This does not turn diagnostic image statistics into universal
visual acceptance.

`build/grass-puddle-perf/Grass-20260930-113457/results.json` records three
alternating off/on pairs on the same clean installed `a001b55f` pair. Each run
warms for 40 seconds before a single screenshot at the exact owner pose; all six
exit normally, with identical 112,164 near / 18,466 mid grass instances. Native
1280x720, MSAA 4, DLSS off, noon and dry terrain are fixed. No concurrent game,
GPU tests, deployment or heavy builds ran during this campaign.

Median final grass-colour GPU snapshots are **3.738 ms off / 4.018 ms on**
(+0.281 ms); prepass snapshots are **1.831 / 1.841 ms**. The median CPU-ring
frame p95 is **14.662 / 15.252 ms**. Colour deltas are positive in all three
pairs; retain the filter for the demonstrated aerial stability improvement,
while recording its cost. These GPU values are terminal snapshots, not
whole-run GPU means. The 256-frame CPU ring includes screenshot readback; its
roughly 199 ms maximum is not a normal-frame regression metric. Object LOD and
shadow triangle counts vary between runs, so this is not an isolated FPS claim.
The runner keeps full samples, installed provenance and hashes rather than
reducing the campaign to its cheapest frame.

## Improved execution order

### Native authored blade follow-up and rebuilt game

The native upload now computes eight opacity-weighted linear RGB layer means
from the actual cropped PlantMat BCR/opacity images. Fully transparent colour
does not contribute; invalid or empty layers keep the original sampled colour.
Only unresolved shading blends toward these means, before the existing ground
tint and gust treatment. Coverage and geometry are unchanged. All **31** grass
tests pass, including actual GPU helper checks and 1x/4x pipeline creation.

Clean installed `2a15d74e` captures retain visible close blades at
`9514.25 2994.25 211.72 90 -18` and populated aerial grass at
`9514.25 2994.25 244.72 90 -86`. The latter uses wind `12 90 0.7`.
The native ON view is darker and more coherent than OFF, while close blades
retain their shape. Scheduled deltas do not establish a native shimmer benefit:
the aerial >48 fractions are 1.013% ON / 0.783% OFF, including legitimate gust
motion and different startup phases. The switch also controls unresolved normal
lighting, so these images do not isolate the new albedo means. Retain the
mathematically checked source change provisionally and use a frozen, zero-wind
comparison before changing source colours in response to displayed brightness.

The full rebuilt game and renderer `77299a74` pass the exact owner camera again
(`corpse-build-regression-OwnerEveron-1-20260930-124148`): normal exit, unchanged
pair, >48 scheduled changes 0.0781%, mean delta 2.811, mean luma 105.930.
The native follow-up therefore preserves the demonstrated OFP aerial fix.

### Frozen native appearance judgement

Clean installed `77299a74` campaign
`corrected-stream-static-20260930-131306-6e832d` compares OFF/ON under identical
paused time 100, date/hour, zero wind, fixed exposure, clouds/terrain UV jitter/
temporal effects off and native PlantMat blades. Both arms exit normally and
confirm the requested filter mode. Startup placement witnesses agree at
20,939 near / 6,227 mid; the harness has no per-pose population query, so these
are not invented close/aerial counters. Static repeats and camera returns in
both arms differ by at most one display level, with means at most 0.000004.

Root's delegated judgement retains the filter: foreground blade shapes remain
visible and the aerial view stays textured, without a conspicuous dark sheet.
Middle-distance grass is darker/more coherent. Central image mean RGB channels
are close OFF 114.141 / ON 104.805, aerial OFF 133.397 / ON 132.292; these include
terrain/objects and are not opacity-weighted grass photometry. The whole filter
changes normals and albedo jointly. This accepts the bounded current look for
continued building, with later owner tuning; it does not prove native shimmer
improvement from static images or every gusting rendered frame.

The preceding `native-static-20260930-125901-098f8e` remains failed: its script
redirected stderr alone, causing Windows bootstrap to reconnect both CRT streams
to the console and lose mandatory mode proof. Redirecting both streams matches
the working benchmark harness; no rendering change or weakened proof was used.

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
   The completed [stock corpse articulation assessment](PHY-040-stock-corpse-articulation-assessment-20260930.md)
   confirms Box3D joint support but finds nonrigid authored death transforms and
   separate rendering/interaction palettes. A render-only override is not safe;
   the bounded dry-ground single-player slice needs shared corpse pose ownership.
   The [stock model admission census](PHY-040-stock-rig-admission-20260930.md)
   now measures all 22 retail person models and 241 LODs, excludes attachment
   helper triangles from anatomical fitting, and refuses uncalibrated joints.
   The missing torso anchor and oversized clothing envelopes remain actual
   rig gates; finite capsule fits alone do not justify enabling a solver.
4. Reassess still-open modernisation tasks against the current ledger after the
   owner-priority visual gates. This update does not enable parked streaming,
   ocean, experimental paging or previously rejected foliage defaults.

Parallel agents use separate worktrees and disjoint files. Only root deploys or
runs the game; an owner-started game is never terminated for testing.
