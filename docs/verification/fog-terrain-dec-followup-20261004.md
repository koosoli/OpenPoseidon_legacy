# Fog height, lighting, Everon horizon and Dec follow-up

Finite owner-authorised batch on `codex/fog-terrain-dec-followup-20261004`,
based on `8479c6f3`. Installed EXE/renderer source: **f8127fe5**, deployed
2026-10-04 11:41. Subsequent commits add smoke tools and evidence only.
Root owns deployment and all installed runs. The owner has not reviewed this
batch. [Curated runtime receipt](fog-terrain-dec-followup-20261004-runtime.json)
retains binary hashes, settings, actual source/coverage, image hashes and fresh
published timing samples. Local screenshots remain in the referenced build
directories; retail world data and extracted art are not committed.

## Changes and owner checks

| Change | How to check / boundary |
| --- | --- |
| Visible bottom/top, thickness, edge softness and density controls for two fog layers, with enabled switches and height reference. | **Ctrl+F8 > Weather > Fog**. Choose above-ground or sea-level height. Sliders apply immediately. Ctrl-click enters an exact height. Minimum band thickness is 0.25 m. |
| Compact mist preset, plus valley/mountain and patch controls. | Set Weather fog above zero, select **Compact mist band**: layer 1 is 1..5 m above native ground with 0.35 m boundary feathers; layer 2 is disabled. Increase density for opacity; reduce softness for a sharper boundary. The far visibility closure still hides distant object culling. This bounds the added local volume, not every pre-existing distance-fog contribution. |
| Fog absorbs incoming main sunlight on terrain and objects; mountains obstruct shafts. | Expand **Fog terrain, patches and lighting**. Change **Light scattering** and **Sun-ray focus**, observe hills/objects looking toward the sun. Ground-following banks/concavity and world-space patches shape the density. This is finite single scattering, not fluid flow/deflection or bouncing photons. Grass/water incoming-material hooks are outside this slice. |
| Coarse opaque terrain continues beyond the former terrain rectangle/CPU far cutoff. | On original Everon, use freefly from an aerial coastal view, dry weather/fog 0 and VD 3500. Fly higher and sideways: distant land should stay opaque. Startup `WGR_TERRAIN_BACKGROUND=0` restores the old selection for comparison. Near LODs/material loading retain their original policy. |
| Safe additions from the two new Dec branch tips. | See the [complete Dec feature and test table](dec-followup-20261004.md): swim/fall source, underwater diagnostic, authored 4WVR materials, local Forest conversion tools, procedural underground fixtures. No compatible swim pack or Forest extraction is supplied. |
| Existing Box3D infantry ragdoll remains default ON. | **Ctrl+F8 > Physics > Automatic infantry ragdolls** controls it. The earlier local angular-velocity guard fallback remains documented; this fog/terrain batch makes no new ragdoll solver change. |

## Installed verification

- Combined committed Game/Tests build passed (212 steps), then metadata build
  passed (15 steps); matching deploy through the game lock.
- Native terrain/infantry checks: **12 cases / 138,631 assertions PASS**.
  Physics/corpse checks: **76 cases / 14,480 assertions PASS**. The first filter
  also requested an absent `[ragdoll]` tag; the separate actual
  `[automatic-ragdoll],[physics],[corpse-pose]` suite supplies the physics result.
- Fog source slice: 10 focused checks, one resource/layout check and two geometry
  checks passed; seven actual production shader compositions passed. The actual
  compact-band shader regression at camera 75 m improved ground solar T from
  0.956543 to 0.924316 (expected 0.924687), and body-height T to 0.940918
  (expected 0.941058). See [source evidence](../fog-incoming-light-20261004.md).
- Full installed renderer pipeline accepted all new resources on this adapter.
  Normal exits, stable binary pair and no GPU validation/panic/device-loss
  errors in completed arms. Minimum-16-texture adapter compatibility is not
  established by this run.
- **18 accepted Everon terrain views**: six old-cutoff, six new extended,
  six new map-only. Camera/weather/render configuration are controlled;
  source is untouched `eden.wrp`, eager material path. Requested VD is 3500
  or 50000. Small published altitude-derived range differences (<0.18 m) are
  retained; exact equality is not asserted. New upper view selects 93 patches
  versus 35 legacy, 58 outside the old rectangle. Actual inspected upper,
  moved, high, wide, near and fogged default views have opaque coast/land;
  inspected map-only upper/high retain authored extents. Legacy upper reproduces
  the owner's pale missing-land silhouettes; new upper removes them.
- **24 fog views PASS**, including moving render camera, OFF/New and zero-weather
  controls. **12 compact-band views PASS**. At fixed 1280x720, 4x MSAA, no
  upscaler, exposure 0.6, both compute dispatches' fresh fog timer samples were
  0.580..1.070 ms for the broad band and 0.473..1.015 ms for compact mist.
  Fogged whole-frame samples were 8.745..9.774 ms broad / 9.251..9.602 ms compact.
  These asynchronous samples are not screenshot-frame identities or a steady
  performance benchmark. CPU windows may retain startup/earlier-view work.
  Do not sum inclusive timers or infer a general FPS guarantee.
- **Two procedural content fixtures PASS**: actual `UgCave` / `UgBasement`
  config/model identity, at least six LODs, native admitted underground hole,
  real roadway/camera floors at origin minus 5 / 3 m, unchanged native source,
  six screenshots, normal exits and unchanged PBO hashes. Interiors are dark
  prototype rooms; AI traversal, rifle combat and Box3D cavity contact were
  not tested here. No final visual-quality approval is inferred from floor checks.

The first terrain runner stopped on an asynchronous screenshot write race.
Its retry completed both extended arms, then stopped on an excessive map-only
high-altitude assertion: the altitude-expanded old rectangle already covered
the whole authored map. The corrected independent map-only arm passed six
views. Failed receipts are preserved, not relabelled as passing campaigns.

## Optional underground fixture launch

The actual generated mod is in `build/sinkhole-test/@OP_SinkholeTest`; it is
opt-in and does not replace retail archives. From this branch's root, with the
game closed, launch the installed `OpenPoseidon.exe` against the game directory
with `--render=wgpu --window --dev --mod "<absolute worktree>/build/sinkhole-test/@OP_SinkholeTest"`
and `--test-mission "<absolute worktree>/dev-missions/ugcave-test.Eden"`.
Use `ugbasement-test.Eden` for the smaller fixture. The cave radio commands test
enter/leave behavior manually. `Prepare-SinkholeTestMod.ps1` regenerates the
two local PBOs; full supplied cave-model regeneration is not claimed because
the supplied visual/memory LODs differ from the generator (details in Dec audit).

## Held content and remaining limits

All 98 prior Dec tips were unchanged/ancestral; exactly two new tips contain
26 unique incoming commits. Safe source/content was selectively ported, rather
than joining histories containing held binaries. **19 swim RTMs plus pack
activation/build/deploy workflow are held** because the new pack contains
conflicting Arma-derived/procedural provenance records and unavailable external
inputs. They are absent from integrated history/current tree. This is a specific
unresolved permission/source question, not a legal infringement finding.

Forest tools require a separately supplied extraction; no Forest world or art
was installed. Broader canopy-card quality, all-map cost, actual fog widget
interaction, moving fluid fog, cave AI and Box3D underground contact remain open.
Earlier unverified floating-house identification and ragdoll local-hit fallback
are preserved. Whole Preview/Compat ledgers have older stale hashes outside
this slice; they are not silently renewed or claimed fully green. This finite
batch ends here rather than starting unrelated roadmap work.

The moved aerial image also shows a thin distant water/extent line. Its source
is not established here; continuous opaque land is the accepted correction,
not a claim that every far-water seam is resolved.

Final owner handoff at 12:02:58 leaves installed source f8127fe5 open in the
ordinary Everon rifle scene (PID 5104 at handoff). Actual SDL firing consumed
30 to 29 rounds; automatic ragdoll default ON and default layered fog were
read back. Six clear targets and the ready view were inspected. Local receipt:
`build/owner-ragdoll-fog/20261004-120258/session.json`. No script-triggered victim
kill is claimed by this handoff. Later evidence/tool commits do not change the
installed game source; `git diff f8127fe5 HEAD -- engine apps content tests/unit tests/content`
is empty.
