# Rotor land ring: flat original Noe source control

The weak e5445111 ring at Noe 2700/5150 is primarily an invalid large-footprint
positive control. Keep the one-metre terrain continuity refusal, full puff
corridor shelter proof, 32 probes per quarter second, allocator and all wet,
road, sea and snow controls. Measure the current profile on flat source ground
before increasing optical mass. This audit changes no production appearance,
game placement, flight input or accepted water downwash.

## Installed evidence and diagnosis

Root-owned `build/rotor-land/soft-profile-own-allocator-20261001-051810-0fe5f6`
has normal exit zero, dry/snow/roof stages, but overall **failed** the later
real-rain wetting gate. Its dry and deposited-snow screenshots have no convincing
continuous visible ring. At actual heli X2699.62/Z5149.56 the trace admits only
one or two puffs per pulse, with live approximately 12–15 and `sheltered=0`.
Neither those stills nor a source calculation accepts appearance.

The actual stock `data3d/uh-60.p3d` ODOL7 model has authored bounding sphere
13.7789554596 m. `Object::GetRadius` and the rotor producer use 0.8 of that value,
so the ring radius is 11.02316437 m. Its candidate centre can reach 95% of that
radius; the expanded billboard and bounded outward travel require another
7.8840634 m. The maximum tested support radius is therefore **18.35606939 m**,
not the old small particle's footprint.

The tiny CPU executable in `Audit-RotorLandProfile.py` calls the actual current
`RotorGroundRing` and `RotorLandProfile` helpers. Sampling the original Noe FMD1
float height grid with its actual anti-diagonal triangles over 128 phases gives
exactly 64 one-puff pulses and 64 two-puff pulses at the observed position. The
perimeter differs from each candidate centre by up to **1.57523727 m**, so the
production one-metre refusal accounts for the missing sectors. All queried
source quadrants are `SandDark`, `files=pt??????`, `soundEnviron=sand`, authored
`dust=.62`; mixed low-dust material is not this failure.

This is a source-only upper bound. It omits actual road and water queries,
collision spheres and ViewGeometry rays, current terrain deformation and shared
budget contention. Matching the observed one/two sectors is useful diagnosis,
not a live collision or native rendering certificate.

## Concrete flat control

Run the unchanged installed runner with **`-SandX 2475 -SandZ 5075`**. Original
Noe source cell `[49,101]` has all four corner heights **23.9849987030 m**, source
texture `o\pt.paa`, and all four SandDark quadrants with empty character. The
18.3561 m worst-case ring/support footprint remains within this flat 50 m cell,
leaving **6.64393 m** to each cell edge. All 128 source phases admit six complete
five-point candidates; the seventh correctly exhausts the unchanged 32-probe
budget. This is six admissions per pulse, not a promised eight-puff emission.

The nearest original retail placement centre is 97.7500 m away:
`o\skal\skala3_4.p3d`, X2555.7119/Z5130.1421. The next two are 99.7350 and
106.3814 m away. Exporter ordinal IDs in the JSON are not live world IDs. These
centre distances are a useful source screen, not model-bound clearance proof;
the installed broadphase, physical support and rays must still pass. The runner
must retain neutral actual auto-hover, strict eight-probe roof suppression,
continuous conservative canopy clearance, removed-roof positive, actual wet
ground, engine-off, high-altitude, pause and existing sea-water controls.

## Optical source audit

Actual `CfgScenePreload.CloudletBasic` uses `data3d/cl_basic.p3d`, whose one quad
references animated `data/basic.06.paa`. `TextureBank` starts at the authored .06
frame and stops at the first absent consecutive frame; the actual retail
sequence is **.06 through .16**, eleven 64x64 sheets. CPU decoding gives mean
alpha **.32031–.32845**, maxima **.85098–.87451** and approximately 72% supported
pixels. There is no empty alpha frame or unproved sprite replacement here.

At RPM one, 9.7 m hover clearance and dry SandDark, the actual grown profile at
age .9 s has dust radius 3.10 m/vertex opacity .07410 and snow radius 2.70 m/
vertex opacity .08040. Multiplying the stock sheet gives mean alpha about
**.024 dust/.026 snow per puff**, before fog, lighting, overlap and depth.
Expansion conserves `radius² * opacity`; changing its strength now would also
increase mass on a correct flat site. The existing custom decal uses the same
weather/eye material lighting as `Object::DrawDecal`; its weather-lit cloudlet
RGB followed by decal lighting is an established path, not a proven new double
lighting regression. Inspect a complete flat-site ring before changing it.

The periodic striped snow ground in the current picture is a separate ground
wind/deformation presentation issue; this particle-source audit does not claim
to fix it.

## Reproduction and checks

Exact source, helper, model, decoder and texture hashes, all profiles and source
counts are in `../evidence/rotor-land-flat-source-audit-20261001.json`. The raw
stock model and decoded sheets stay in ignored scratch output, not new assets.
Use an existing CPU export directory produced by
`Audit-OriginalOFPContactSources.py`, including its FMD1 sidecars:

```powershell
python scripts/Audit-RotorLandProfile.py `
  --source-audit <original-map-audit-directory> `
  --game-dir 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault' `
  --tools <matched-PoseidonTools.exe> `
  --out build/rotor-profile-audit
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Test-RotorGroundPolicy.ps1
```

The source audit completed twice against current c99b8d17 helper source; its
production CPU probe compiled with `-Wall -Wextra -Werror`. Canonical rotor
ground policy/hydrology/annulus and dedicated allocator source contracts passed.
Python syntax and `git diff --check` passed. No game, GPU, full build, install,
profile mutation or deployment was run by this agent. Flat-site installed
appearance and the overall real-rain lifecycle remain pending with root.
