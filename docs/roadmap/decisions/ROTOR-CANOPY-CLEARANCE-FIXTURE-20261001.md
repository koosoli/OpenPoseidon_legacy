# Lower stock canopy for the land-rotor negative control

Source-only candidate from `c6822ed3`; installed campaign still required.

The actual `neutral-look-ground-spool-20261001-030423-ab0d44` campaign exited normally but failed its bounded-position condition during **roof-covered** observation. The dry and deposited-snow stages each ran more than eight simulation seconds and retained four emission rows: eight particles per sampled pulse, 48 live. The aircraft remained near 2699.73/5149.49, with RPM 1 and nearly unit-up orientation. This was not a failed dry hover or proof of a land emitter defect.

Immediately before the canopy, actual helicopter origin Y was 24.76660 and ground was 14.98500: only 9.78160 m clearance despite the requested 18 m stage placement. After the canopy appeared, the first recorded up vector became approximately (-0.15024, 0.98787, -0.03923); the next was (-0.28634, 0.95671, -0.05219), with substantial horizontal speed. Manual driver authority, no camera effect, no player suspension, Q=0 and moveUp=0 remained recorded. The older installed snapshot did not yet expose the auto-hover flag or actual pilot requests, so those cannot be retrospectively asserted.

## Geometry evidence

The audited stock `stan_eastc.p3d` has source vertical bounds -2.81005287 to +2.81005287 m. Its actual source hash is `C7505A2B08231A540147515D429AF3083CDCCB9123CB93F3B0B5FCBFA6B8D696`, matching the earlier stock roof decision. Helper placement with RoofLift 3 put the central model origin at 20.81510 and its upper bound at **23.62515 m**. Terrain-following outer tent origins on the hillside put their upper bounds as high as **27.10045 m**. Thus canopy geometry occupies the actual aircraft's vertical range. Removing only the three-metre lift leaves some hillside tent bounds as high as 24.10045 m.

Read-only extraction of actual stock `uh-60.p3d` from both installed DTA and Remastered/DTA archives gives the identical hash `A3ABDCDE4ECE7B7260F9AC9DD2F0F0B87EF1545CE8F5196D55B879E3A86DBAE7`. PoseidonTools identifies ODOL revision 7 with 17 LODs. Its actual Geometry LOD 10 has 172 source points and 178 faces. The engine-reader position dump gives min (-4.34560,-2.44864,-5.80734), max (4.58090,2.46579,10.28430). At the measured upright origin, its lowest original geometry point is approximately **22.31796 m**, below the central lifted tent's upper envelope. These are bounded stock source facts and a plausible collision diagnosis, not a logged triangle-pair collision proof.

`Helicopter::Animate` expands each LOD's bounds by 1.1 around its sphere centre. The fixture uses a deliberately conservative envelope: minimum model Y -3.00 m and horizontal radius 13.20 m. Those exceed the 10% expanded Geometry bounds even when allowing the expansion centre anywhere inside their original bounds. With unit-up component `u`, its world-space lower envelope is:

`originY - 3.00*u - 13.20*sqrt(1-u*u)`.

This accounts for tilted X/Z extents without guessing yaw. It is a fixture clearance bound for the audited stock primary helicopter geometry, not certification of arbitrary addon models, attached cargo or animated proxies. The helper rejects another model or a non-unit/non-upward rotation. The source dumps/extracted assets are retained only in the isolated ignored `build/rotor-canopy-audit/` directory.

## Runner change

Only `Test-RotorLandRuntime.ps1` changes. It creates all 25 actual verified CampEastC/stan_eastc roofs with RoofLift 0, then places their origins at the **same actual centre-ground datum**. For the preceding measured site, that is origin 17.81510 m and top **20.62515 m**. Every helper-proven class/model and initial position remains recorded; every relocated ASL coordinate is queried and checked. Outer low wall sections may be buried on the hillside. This is temporary static test-canopy geometry, not a change to ordinary game placement.

The covered stage requires at least 0.35 m clearance between the tilted conservative aircraft envelope and the maximum confirmed roof top before observation and at every approximately 250 ms polling sample. It retains all samples before assertions. The previous upright case has approximately 1.14 m clearance under this conservative bound. A tilt or natural flight movement that consumes this clearance fails the fixture; no aircraft position, velocity, rotor speed, flight helper or physics is forced during observation. These discrete samples cannot prove absence of every excursion between polls.

Every recorded covered pulse must report **all eight actual physical airflow probes sheltered**, and zero emitted land particles. After deleting the 25 real objects, the runner verifies their absence and requires actual dust emission to resume. Roof shape/layout alone cannot establish blocked rays. Existing bounded-position, source, engine-off, snow, pause, high-altitude, real-rain, sea and normal-exit gates remain. Existing ocean water downwash is neither modified nor numerically reaccepted here.

The schema now requires the installed read-only input/wind/hover diagnostic fields. During manual covered samples, it also requires actual auto-hover, unsuspended local manual control, ordinary look-around, and zero movement/turn/pilot-speed requests. Those states are recorded even if rejected. They establish the requested neutral control during the new run, not actual zero aircraft velocity or zero wind. All samples retain actual wind, mouse direction and helper flags for root diagnosis. This runner therefore requires the matched build containing the new getter; an older pair missing fields fails explicitly.

## Checks and remaining evidence

PowerShell `-SelfTest` and `git diff --check` pass. Tests reject missing/malformed diagnostic fields, the original vertically overlapping canopy, tilted underside excursions and nonzero pilot requests. The tilt falsifier also caught integer-overload rounding in initial Math.Min/Max calls; the committed helper uses explicit floating-point bounds. No game, GPU test, production renderer/flight change, full build or deployment was performed by this agent.

Root must run the full serial campaign with the updated installed getter, inspect covered/removed physical ray counts and clearance, and retain normal-exit evidence. The old failed campaign and its valid dry/snow positives remain separate. Particle appearance still needs judgement after the full functional campaign: weak dust, sparse white snow flecks and old ground striping are not resolved or accepted by these fixture changes.
