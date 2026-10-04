# Kolgujev fog and corpse feedback, 2026-10-04

Finite owner-authorised regression batch on `codex/kolgujev-fog-corpse-fixes-20261004`, based on installed stock-character checkpoint `41ce4191`. Root alone owns the installed game folder. Existing improvements and default Physics/Weather toggles remain. This does not reopen the whole modernisation roadmap.

## Changes

- Outdoor mist in front of an opaque rock or building is no longer discarded because that receiver itself shelters rain. Explicit cockpit and native cave-interior exclusions and unknown-source guards remain.
- Thin vertical layers sample scattering where the layer actually participates, rather than at an empty interval midpoint. Eight importance samples replace unstable fixed sky directions at high anisotropy. Exact analytic extinction remains.
- Fog radiance and matching distant/horizon closure use a luminance-preserving neutral grey-white colour policy. Clear weather, high sky, clouds, stars and environment lighting preserve their original paths.
- Nighttime volume encoding no longer depends on daylight. Up to sixteen actual nearby point/reflector lights scatter through the medium, respecting real range, cone, light-path extinction and the available local shadow atlas. This includes the native player torch; it is not a screen-space cone.
- Grass colour fields default to approximately 167–200 metres, with broader continuous transitions and matching distant proxy fields. Fine blade detail, wind and the existing anti-glitter filtering remain.
- Falling snow adds an automatic render-fog contribution from 0.15 to 0.60. Stronger authored/manual fog takes precedence. Stopping snowfall returns to authored fog; the authored weather value is not overwritten.
- Rifle hits on infantry use resolved per-ammunition physical momentum and the real impact lever. The old legacy bullet mass could turn a normal rifle hit into a much larger impulse. Explosions, damage and other vehicle paths are preserved.
- A valid live pose whose measured hinge frame fails can use the existing constrained spherical graph, with all source/contact/finite checks repeated. Grounded automatic poses can recover up to 100 mm of positively identified shallow terrain contact while keeping exact initial skin, hulls, origins and anchors. Every body origin must remain above agreeing native/solver floors; existing motion and supported-retirement limits stay.

## Verification and limits

Native physics/articulation/corpse-pose/snow tests: **32,435 assertions in 91 cases passed**. Fog suite: **15 GPU/production composition checks passed**, including thin support, normalized anisotropic sky energy, opaque receiver transport, chroma/closure, night emitter direction/range/count and actual local atlas obstruction. Production sky creation and its validation test now share the same import-composition helper.

The first installed source `34959cb9` exposed a production sky loader error: it passed a new import directly to the raw WGSL parser. Its aggregate runtime checks correctly failed even though individual physics cases succeeded. `08ecd9a4` fixes that construction path. Those failed receipts are retained and are not promoted to visual/runtime passes.

Actual normal-player rifle test on installed `08ecd9a4`: **all four stock classes passed** (US, Russian, Resistance, Civilian), both fresh active corpses and supported frozen corpses. Tests use ordinary SDL mouse aim/fire and observe ammo consumption, native hit/wake counters and a changed consumed pose; no scripted projectile is used. Original Everon, unchanged paired binaries, normal exit, clean GPU/runtime logs.

The hillside prone witness is deliberately partial. A civilian recovered 75 mm of actual initial overlap and retained a supported frozen pose; the final Western toggle fixture recovered 47 mm. Other Western/Russian poses overlapped the ground 179–181 mm and correctly refused. Controlled full-rig tests cover 36–100 mm overlap in both joint modes, but do not establish all retail stances. Two previously recorded Camel-pilot post-hit settling failures, frozen saves without wake recipes, four-active/64-retained budgets, multiplayer and arbitrary addon coverage remain outside this batch. Do not claim universal all-character/all-posture acceptance.

The exact owner black-spot appearance was not reproduced pixel-for-pixel. The corrected supplied Kolgujev view is checked with dense and three-metre bands; the earlier high authored-fog baseline obscured too much geometry for an honest identical-profile before/after comparison. Shader counterexamples establish the repaired mathematical defects independently. Broad canopy, streaming and every reflective material path are not newly certified here.

Runtime receipt and final visual observations are recorded alongside this document after the installed checks. GPU timing snapshots are asynchronous and are not a statistical benchmark or a same-screenshot-frame attribution.

Installed visual observations: original Kolgujev dense mist is less blue and the inspected thin-band sky does not show the owner's black spots. Grass top/medium captures show broad blends; the underlying original terrain texture and canopy detail remain. Night/runtime/actual torch input checks exit normally, and the storm fixture reports effective fog0.60 with authored fog0. These are distinct from a strong-shaft appearance pass: rear, front/side, native-camera and shadow-disabled captures remain very dark and do not convincingly establish the requested larger torch shaft. That visual quality item is explicitly **open**. The isolated GPU torch direction/range/atlas tests establish the transport math, not that artistic acceptance.

A final clean-graphics prone witness on08ecd9a4 refused a Civilian pose at105.63mm actual overlap, beyond the100mm guard. Its separate Western fixture admitted39.05mm and passed supported freeze, retention and cleanup. Animation phase changes the actual initial overlap; this is not a repeatable all-prone pass.

Installed binary source remains08ecd9a4, stamped19:04 local. Later 2d26865c and 89ef58e6 change only conditional test fixtures; documentation commits do not restamp or pretend to be a new game binary. Evidence JSON includes the exact pair hashes, successful four-class player-fire receipts, and partial/failed witnesses. The finite batch stops here at the owner's requested wrap-up point with the limitations preserved.

Final full sky suite on 89ef58e6: **42 passed, 0 failed, 1 ignored**. The two reflection fixtures now compose the same production imports for the original and budget/interval control shaders; their existing GPU assertions remain. Production geometry/lighting code is unchanged by these final test-only commits.

