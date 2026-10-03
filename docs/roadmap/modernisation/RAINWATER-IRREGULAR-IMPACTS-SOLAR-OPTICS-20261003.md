# Rainwater irregular impacts and solar interface candidate

Scope: RainWater's shared coarse/fine fragment only. The accepted Tidewater sea,
coastal wetness, terrain film, source admission and hydrology are unchanged.

The prior packet placed one impact inside the central 10% of each 0.8 m cell
and repeated every 0.91 seconds. The actual Nogova top capture from
`unedited-original-hollows-20261001-062817-9a1851/noe/on` shows rows of circles.
That repetition is a shader cause independent of the rectangular water geometry
visible in the same image. This candidate does not fix the geometry or drying.

The replacement uses independently hashed event centres, periods (1.4–2.7 s),
lifetimes (0.55–1.05 s) and amplitudes. Centres vary between events. Nine bounded
neighbour cells allow rings to cross tile boundaries continuously; the maximum
radius is 0.29 m and radial support ends at 0.34 m inside a 0.75 m cell lattice.
Temporal envelopes fade completely before a centre changes. Footprint widening
also reduces slope energy, and all impacts vanish by a 0.16 m pixel footprint.
There are no textures, publications, global state or unbounded searches added.

`frame::ground_sky_reflection` deliberately bakes the shared atmosphere without
the sun disc. RainWater previously had no separate direct specular sun term.
The candidate adopts Tidewater's dielectric Fresnel, GGX distribution and Smith
visibility equations for this term, using the same irradiance/pi conversion,
CSM, terrain and cloud shadow inputs. A minimum slope width of 0.08 and normal
derivative variance broaden the lobe before it can resolve into sparkling
pixels. This width is a material calibration, not a measured roughness value.
No sky/body multiplier, arbitrary HDR colour clamp or physical volume change
was introduced. The existing energy-compositing surface helper is unchanged.
Direct specular is added through the same straight-alpha coverage exactly once,
without multiplying its own Fresnel by the environment Fresnel again.

The existing rough-substrate film at <=8 mm, smooth 8–50 mm handoff, support
depth, source epoch, native triangulation, ownership, sea and snow negatives,
actual shelter proof and mass accounting are preserved.

Validation in the isolated source worktree:

- Actual pure module Naga validation and complete RainWater composition pass:
  `rain_water::optics_tests::actual_thin_water_helper_is_pure_and_full_fragment_preserves_support_guards`.
- `git diff --check` passes.
- A new actual device test supplies 64 spatial/time samples, cell-boundary
  comparisons, zero-rain/distant negatives and direct solar helper probes.
  Its GPU execution is pending the root's serial validation campaign.
- Full build, installation, runtime performance and visual acceptance are pending.

Required root checks: focused/all renderer GPU tests, existing coarse/fine
physical draw/support negatives, actual low/top rain views on the same retained
native pools, pause/return stability, dry and >0.16 m footprint negatives, direct
sun versus cloud/shadow views, thin-film depth fixtures and a rain helicopter
performance comparison. The nine-neighbour work is skipped at unresolved
distances, but close water fragments and solar shadows still need measured cost.
Ground film still uses the older ripple helper; sharing the revised effect with
it is a separate follow-up outside this candidate's file ownership.

## Required-device fixture update

Root's combined device run passes the new64-case impact/solar helper. Two
pre-existing tests require source-fixture maintenance: the camera source proof
still names the replaced ripple function, and the full draw camera layout lacks
the now-used production shadow/cloud bindings. The repaired source proof checks
the new impact, sun and shadow helpers without changing grid or draw bounds.

The complete draw fixture now binds actual frame slots1/4/6/13, including the
64-byte terrain/cloud map uniform and independent cloud transmittance texture.
Its old support/ownership/roof/snow/sea/underside negatives are unchanged. A new
real fragment counterfactual aligns the sun with the flat pool reflection and
changes only cloud red transmittance from1 to0. It requires identical alpha,
lower solar RGB energy and retained non-solar reflection/body in the shadowed
arm. CPU compilation and the camera/composition test pass. Execution of the
repaired complete device test remains root-owned and pending.
