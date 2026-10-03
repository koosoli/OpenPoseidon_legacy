# Native pool observation comparison

`scripts/Compare-RainWaterNativeHeads.ps1` reads two completed fine ON natural
runtime `result.json` files. It does not launch a game, compile code, alter
sources, infer missing depths or alter water. Example after the next campaign:

```powershell
& scripts/Compare-RainWaterNativeHeads.ps1 `
  -Baseline 'build/rain-water-natural/actual-solar-native-owned-pool-20261003-133514-9e5936/result.json' `
  -Candidate 'build/rain-water-natural/<completed-new-campaign>/result.json' `
  -Map noe -Output 'build/rain-water-natural/native-head-comparison.json'
```

The inputs must retain their recorded native tile files at the recorded paths.
The comparator verifies the corpus SHA, unchanged original source, complete
1,024-centre native owner, normal uncontended exit, stable installed pair,
paused clock/field/weather, matching corner/query revision and exact native
vertex/centre domain. It checks each actual query's height/depth/support against
the anti-diagonal centre bed within float-query precision. Changed native bed,
source world/WRP or domain is rejected. Each corpus sea level must equal its
own actual fine snapshot's sea level. Fresh sessions can have different dynamic
sea controls: that comparison is allowed only for the same complete inland
owner, with every actual native vertex unchanged and the minimum native bed
above both sea levels plus the actual 2 cm surface-support margin. Any possibly
coastal/submerged unequal-sea comparison is rejected.

The full corpus SHA is checked against its own recorded manifest. A separate
SHA hashes only the native `(x,z,height)` geometry using ordered binary values;
this excludes installed provenance, clock, dynamic sea and fresh process
addresses. All overlapping actual native vertices must also match exactly.
Process world tokens are recorded; source revisions are checked within each
run. Two fresh process addresses are not a cross-run identity guard.
Unequal sea explicitly marks the report noncausal: global coastal outlets can
affect distant retained volume even when the sampled inland owner is safely
above both sea thresholds.

Each snapshot reports centre-wet adjacent pairs, head-step mean/RMS/p95/max,
counts over 30/60 mm, the wholly wet native-face subset, velocity jumps and a
four-wet-neighbour interior curvature/extremum observation. A local extremum or
step is an observation, not an automatic failure: physical incoming flow, a
native rim or transient gradient can produce it.

Across snapshots it matches exact centre/face coordinates and native beds;
only pairs wet in both states enter matched step statistics. It also reports
wet/dry transitions. If natural selection chooses another owner, it reports
`observations-only-no-common-wet-pairs`. It does not compare unrelated pools as
though their distributions proved improvement. Current natural runtime picks a
deep owner, so a changed solver can legitimately select a different location.
The snapshots include actual rain/weather, time, field mass/history and owner
volume, needed when interpreting the observations.

## Executed checks

- `-SelfTest` passes: known checker head differences, a level comparison,
  unrelated owner with zero matched domain, and changed-bed rejection.
- Reading the actual `9e5936` input against itself passes every provenance,
  domain and native support gate. It reports `sameInput=true`, 1,024 common
  centres and 41 matched wet pairs; mean step 0.0394633223371 m, maximum
  0.077880859375 m. Its wholly wet native-face subset has 35 pairs. This is a
  same-input parser check, not a newer-build result.
- `git diff --check` passes. No compiler, device, game or visual acceptance run
  is part of this comparator change.

The first actual new-versus-old invocation correctly exposed differing dynamic
sea controls but originally rejected them too broadly: old `9e5936` sea is
2.714284181595 m; new `7a5b18` is 2.867092847824 m. The corrected observations
preserve each sea source and reject any possibly coastal unequal-sea domain.
Expanded self-tests pass for fresh inland sea, and reject changed WRP/world,
range/spacing, geometry SHA, individual outer native vertex, low/coastal bed,
centre bed and a mismatched same-run sea source.

Actual completed new campaign:
`build/rain-water-natural/stable-native-sunny-owned-pool-20261003-144426-7a5b18`.
The old/new full corpus SHAs differ, but all 1,089 vertices and 1,024 centres
match, with geometry SHA
`49AAA67A5D60A93BD1850E7434E2BB6BCDDF245C25315855E867A40588D83CDD`.
Minimum native bed 197.8514404297 m clears the higher sea by 194.9843475819 m.
The report is `matched-native-observations-unequal-sea-noncausal`.

For the same 31 centre pairs wet in both snapshots, old/new head-step means are
0.04412497244 / 0.01203524682 m, maxima 0.07788085938 / 0.03857421875 m;
counts over 30 mm are 26 / 3, and over 60 mm 5 / 0. The same 29 wholly wet
native faces have means 0.04394636483 / 0.01218598464 m. The snapshot-specific
four-wet-neighbour interior stencil counts are 9 / 6, mean curvature
0.04254192776 / 0.00376129150 m and extrema 9 / 0; these differently sized
stencil sets are descriptive observations, not a matched temporal test.

Actual rainy sample conditions differ: liquid rain 0.4692493677 / 0.3204274178,
owner volume 1131.376113 / 925.938753 m3 and simulation time
127.738 / 129.064 seconds. This is not an equal-forcing causal ablation or a
visual/performance acceptance statement. No runtime/source/hydraulic change
was made to enable this comparison.

## Remaining optics observations from existing source

The accepted Tidewater coast wetness (`terrain/terrain.wgsl`) obtains a wet
channel from shore simulation or its bounded coastal band, then darkens the
textured substrate by 0.58 with modest saturation/cooling. Copying its sea-level
band would not produce physically sourced inland rain pools. The implemented
wet-soil handoff already reuses its substrate darkening while keeping the rain
and native-support guards.

The rain-water sun helper uses Tidewater's dielectric IOR 1.333 and GGX/Smith
equations, with derivative roughness. Its independent sun contribution matches
the shared sky bake's exclusion of the sun disc. Tidewater's roughness also
accounts for unresolved wave slopes and its reflection path has horizon/SSR
confidence handling. These are useful mechanisms, not a reason to increase
the top-view Fresnel coefficient or copy its arbitrary specular clamp.

Bounded issues to review after the new geometry/state captures:

1. **Body lighting versus visible shadow.** `rain_water_body_radiance` receives
   direct sun without the CSM/terrain/cloud visibility later used for the
   separate solar interface. Its sky contribution also has no local sky
   occlusion. This can leave body radiance beneath a tree brighter than the
   source-qualified surroundings. A future narrow change should carry actual
   visibility into the corresponding lighting components, preserving ambient
   and reflected radiance; device negative controls must prove shadow/cloud
   removal affects only direct light.
2. **Deep top-view body domination.** Current depth attenuation is
   `exp(-3*depth)` and body reflectance is `(0.045,0.060,0.052)`. At the actual
   2.1838 m focus, vertical transmission is about 0.00143, so a near-vertical
   view is mostly the grey-green body, with approximately 2.5% interface
   reflection. A flat pale pool in that view is therefore partly predictable
   from the present optical law, not proof of missing SSR. Tidewater integrates
   a refracted path toward a floor and separates transmission/scattering.
   Changing absorption/body colour requires a defined fresh-water/sediment
   contract and paired native-depth tests; no arbitrary brown tint, global
   reflectivity increase or camera-dependent workaround is proposed here.
3. **Time-sensitive flow phase and cell shading.** Fine water still publishes
   one velocity per cell. Flow packets use `worldXZ - velocity*time`; changing
   velocity therefore changes phase by `time*deltaVelocity`, and adjacent
   velocities can produce shading seams even after head steps settle. The
   accepted derivative fade bounds distant detail, but it does not establish
   temporal continuity at near/middle distances. Inspect matched actual flow
   jumps and moving captures before considering a bounded phase formulation.
4. **Reflection filtering is primarily fading.** Rain SSR uses the actual
   opaque scene/depth, bounded march, hit validation and sky fallback. Increasing
   derivative roughness fades SSR confidence rather than convolving the scene
   reflection. A future low-cost rough-reflection filter should preserve valid
   black/night hits, offscreen fallback, body exclusion and energy weighting;
   it must justify extra sampling cost with actual captures and device tests.

No optics or rendering code is changed by this observation slice. Physical
solver settling, actual shader lighting and final appearance remain distinct
runtime review gates. Two paused snapshots alone do not prove a time-domain
oscillation or a causal before/after improvement.
