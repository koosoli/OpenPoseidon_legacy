# MAT-041 — Emitter radiance is a target peak, not a multiplier; and the A3 floodlight orb is not the emitter

**Date:** 2026-08-12
**Related:** the owner-reported "A3 floodlights render as huge sun-like orbs"
**Status:** The emitter-radiance semantics are fixed and verified. The reported orb
is **not fixed** — it is measurably a different mechanism, named below.

## The multiplier could never have worked

`StaticEmitterRadianceScale` matched material path patterns and multiplied the
authored emissive by 0.015 or 0.08. Arma 2 and 3 author the emissive lane on a
legacy 1e2..1e6 scale. Measured across all 66 light materials in A3's `data_f`:

```
lamp_searchlight.rvmat            600000 600000 600000   <- what A3 floodlights emit through
car_beacon_red_emit.rvmat         730000 150000 140000
env_flash.rvmat                   200000 200000 600000
building_warninglight_red.rvmat   180000   2000   1000
air_positionlight_red.rvmat       150000    500      50
lamp_fluorescent.rvmat             28500  27400   30000
lamp_street_white.rvmat              300    265     260
```

600000 x 0.08 = 48000. The HDR path sRGB-decodes that, which overflows the
`Rgba16Float` scene target long before bloom (threshold 1.0) sees it. To land at
the knee the multiplier would have to be ~1.7e-6, four orders of magnitude below
either constant. **No hand-picked absolute multiplier can work**, because the
constant is absolute and the input is not.

Worse, the pattern list keyed on the wrong materials: every `\lamps\` and
`lamphalogen` RVMAT measures emissive **0**, so the tighter 0.015 arm was a
measured no-op, while the one material that actually emits sits in the looser
0.08 arm via `\lights\`.

## The fix

The family constant becomes a **target peak** and the scale is derived per
material — which is exactly what the retained path already does
(`gpu_driven.wgsl`: `emissive * min(1.0, 0.50 / peak)`). Materials already at or
below their target are untouched, which is every stock CWA material: all of
`CfgMaterials`' emissive values measure `{0,0,0,0}`, so OFP is provably
unaffected. `lamp_searchlight` is added to the 0.015 arm so the floodlight lens
is keyed on the material that emits rather than on the housing that does not.

This also bounds the many emitters that match no pattern at all (target 1.0,
i.e. the bloom knee) instead of letting them overflow.

## The orb is not the emitter — measured

With the fix in, the owner's symptom persists: framed at 35 m, the Stratis
airbase floodlight is still a screen-filling white disc.

Direct probe: **zeroing all direct-path mesh emissive produces a
pixel-identical frame.** The orb does not come from the emitter mesh, so
`StaticEmitterRadianceScale` — the mechanism the handover pointed at — was never
going to fix it, whatever its constants.

Two further observations bound it:

- At normal viewing distance (60 m up, whole airbase in frame) the night scene
  looks **correct**: plausible pools of lamp light on the ground, runway lights
  as small points, no discs. The illumination contribution is sane; the artefact
  is local to the light position.
- `AddA3StaticMarkerLights` (`Landscape.cpp`) creates `StaticMarkerLight` at
  brightness 0.25 and explicitly overrides `FlareIntensity` to 0, so those are
  not it either.

**Next lead**, for whoever picks this up: the light-flare sprite in
`SceneDraw.cpp` (~line 478-520), which draws `DrawFlare` for any light whose
`FlareIntensity` is non-zero — `LightReflector::FlareIntensity`
(`Lights.cpp:469`) is non-zero inside the light cone, and an A3 floodlight is a
config `Reflectors` spot. The WGPU branch there already caps alpha at 0.10 and
size at 0.030, so if that is the source, either the cap is not reached on this
path or `DrawFlare`'s size argument does not mean what the cap assumes. Confirm
the same way this record disproved the emitter: stub the call and re-capture.

## Verification

- OFP preview-0 WGPU gate: passes.
- Stratis night, wide airbase framing: no orbs, correct light pools.
- Stratis night, 35 m framing: unchanged (still the reported orb) — stated
  plainly because the fix does not address it.

---

# Amendment — two more hypotheses killed, and the reproduction is suspect

Continued after the owner reported the orb still present on the Arma 3 map.

**Not the light flare either.** `SceneDraw.cpp`'s `DrawFlare` (drawn for any light
whose `FlareIntensity` is non-zero, multiplied by `GetAccomodateEye()` and
composited additively with `NoZBuf`) was the standing lead and looked strong for a
night artefact. Stubbed behind a scratch env gate and re-captured at the same
framing: **pixel-identical, the orb is untouched.** So it is neither the emitter
mesh (proved earlier by zeroing all direct-path emissive) nor the flare sprite.

**And the reproduction itself is now in doubt**, which matters more than either
result. Two further captures at the same framing:

- **In DAYLIGHT (hour 12) the orb is still there**, the same size and place. So it
  is not night lighting, not eye accommodation, and not bloom-on-an-emitter.
- **From ~140 m the airbase renders cleanly** — no orb at all, and the
  `lamphalogen_f` post is visible as an ordinary thin pole.

A lamp lens is a few tens of centimetres; it cannot fill the frame at 36 m. The
far capture shows large pale concrete structures (a fuel tank, hangar aprons)
around that spot, and a bright surface a few metres from the camera blowing out
under auto-exposure explains a day-and-night, distance-dependent white disc
exactly.

**Conclusion: the framing used here — camera (1707, 5044, 14) looking at object
76383 at (1677.4, 11.5, 5024.7) — is very probably NOT reproducing the owner's
bug.** Chasing it further without a known-good reproduction is guesswork, and two
sound ablations have already been spent on it.

**What is needed next is the owner's own viewpoint**: a screenshot, or the
free-fly coordinates from the Dev Tools Game tab (which has a copy button), taken
where he actually sees the orbs. With that, the same ablation method settles it in
one build. The remaining candidates, in order: a light-volume cone mesh from the
lamp model drawn additively; the local point/spot contribution in `lights_contrib`
at very close range; or auto-exposure/bloom on an ordinary bright surface, which
is not a lamp bug at all.
