# Rain flight: completed measurements and nested CPU attribution

## Evidence and scope

The runner now records and restores `POSEIDON_SIM_VEHICLE_COST` explicitly.
Use `-CloudletCosts` to request nested native CPU accounting in both arms;
the default is zero, preventing an inherited process override from silently
changing this benchmark. Requested accounting must appear in actual complete
logs. Existing measurement, route, source, weather and exit guards remain.
Pure helper selftests pass; the explicit-cost runtime follow-up is in progress.

Installed86aae174 actual88be03 passes both normal stable arms and zero drops:
dry mean9.480935/p9511.456ms, rain mean10.489598/p9513.452ms. This run did
not enable nested CPU accounting; it cannot prove a measured solver saving.
Compared with preceding e0178ffcd5, rain mean10.774874/p9515.105ms, the
bounded independent run is encouraging but not causal FPS isolation.

The installed `bd590703` four-arm campaign completed normally with every arm
passing its route, weather, source-domain, binary-identity and capture guards:
`build/rain-helicopter/occupied-matched-rain-weather-20261003-132025-8b38bb/result.json`
in the `combat-audit` worktree. This audit reads the result, complete frame
capture summaries, measurement-window native logs and published renderer
snapshots. It does not introduce a performance fix or claim one.

The default and far-cover-disabled comparisons each have dry/rain route p95
separation of about 2.5 m, height difference below 0.4 m and velocity difference
below 0.05 m/s. Maximum matched elapsed-time difference is 0.612 s. Actual
overcast, terrain range and object range match; mission and requested-profile
hashes match within each pair. The coarse runoff domain remains 513 by 513,
with 25 m spacing in all four arms.

The dry control uses the finite ordinary native weather transition documented
in `RAIN-FLIGHT-WEATHER-CONTROLS-20261003.md`. It has tiny positive rain
(effective density approximately 4.5e-16 to 7.0e-16; measured water input
approximately 1.85e-10 to 1.89e-10 cubic metres). It initializes coarse water
physics, so it is not an entirely inactive-water-physics control. The existing
dry rainfall budget remains enforced.

## Completed frame measurements

These are genuine ordered, drop-free completed frame captures. Values are ms.

| Arm | Frames | Mean | Median | p95 | Maximum | Frames >20 / >40 / >80 ms |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Default-Dry | 2277 | 10.8725 | 10.701 | 15.392 | 33.472 | 22 / 0 / 0 |
| Default-Rain | 1887 | 12.8300 | 12.113 | 23.992 | 109.158 | 139 / 5 / 1 |
| FarCoverOff-Dry | 2292 | 10.6756 | 10.570 | 14.370 | 47.041 | 21 / 3 / 0 |
| FarCoverOff-Rain | 2115 | 11.8300 | 11.279 | 22.158 | 53.330 | 120 / 2 / 0 |

Rain adds 1.9575 ms to the default mean and 1.1543 ms to the far-cover-off
mean. Both rain arms have a substantially longer tail. The corresponding
difference of differences is about 0.803 ms, but it cannot establish the exact
cost of far cover: the independent runs have different actual rainfall and
water history. Default rain ranges from 0.542229 to 0.698765 and contributes
220478.14 cubic metres; far-cover-off rain ranges from 0.458310 to 0.647550 and
contributes 203178.82 cubic metres. Water flow, puddle and soil states therefore
also differ. Normal native sea level differs by about 0.02 m at queries.

Completed CPU phase means (ms per rendered frame) locate work in both simulation
and drawing. Containers below must not be summed with their nested work.

| Phase | Default dry | Default rain | Far off dry | Far off rain |
| --- | ---: | ---: | ---: | ---: |
| sim:step | 0.531 | 1.109 | 0.479 | 1.072 |
| drw:land | 1.520 | 1.735 | 1.430 | 1.632 |
| drw:obj | 0.014 | 0.357 | 0.013 | 0.286 |
| swap | 7.016 | 7.578 | 7.086 | 6.904 |

The default rain `sim:step` maximum is 28.613 ms; far-cover-off rain reaches
43.574 ms. Default rain swap reaches 104.322 ms, while its full-frame maximum
is 109.158 ms. Aggregate phase maxima cannot prove these are the same frame;
phase quantiles and independent maxima are not additive. No explicit lightning
event was found in the measurement-window logs. Native full-cloud weather can
spawn thunderbolts, but attributing these outliers to lightning is unsupported.

## Existing native cloudlet and rain-volume receipts

Existing `Sim vehicles ms/tick` receipts identify a repeatable increase in the
Cloudlets container. Weighting each native receipt by its actual physics tick
count in the bounded measurement window gives:

| Arm | Receipts / ticks | Cloudlets ms per physics tick | Maximum receipt-window mean |
| --- | --- | ---: | ---: |
| Default-Dry | 24 / 1449 | 0.5756 | 0.690 |
| Default-Rain | 24 / 1451 | 1.1711 | 1.528 |
| FarCoverOff-Dry | 23 / 1389 | 0.5251 | 0.652 |
| FarCoverOff-Rain | 24 / 1453 | 1.2241 | 2.024 |

These are physics ticks, not frames. Existing ground-shadow receipts report
`NCloudlets=0` in all four measurement windows. This does not imply the
Cloudlets container is empty: `World::SimulateCloudlets` also advances snow,
mud, sand and actual terrain runoff.

Rain-volume trace samples (26 per arm) have mean spawn/gather/simulation work
of 0.4398 ms for default rain and 0.3519 ms for far-cover-off rain. Their mean
draw encoding is 0.0758 and 0.1307 ms respectively. Live population is about
978–1272 and 822–1178; actual rainfall differs, so identical populations are not
expected. Dry has no live/drawn rain drops and only microsecond-scale bookkeeping.
`GRain.UpdateAndDraw()` is called in `World/Scene/SceneDraw.cpp`, outside
`World::SimulateCloudlets`; none of the new nested cloudlet counters include it.

## Renderer snapshot limits

The published nonblocking snapshots have no source-frame identity or timing
age. The 20–21 measurement polls can repeat or lag; these are sampled medians,
not a second completed GPU-frame capture. Do not add nested containers or derive
completed GPU-frame quantiles from these values.

Sampled GPU-total medians are 10.6220 → 12.7165 ms (default) and
10.2758 → 12.1907 ms (far off). Default rain near-cover cull/draw medians are
0.1055/0.0922 ms; far cover is 0.1249/0.2427 ms. Far-off rain still performs near
cover at 0.1044/0.0922 ms and correctly omits far cover. Weather encode snapshots
are around 0.004/0.020 ms per cull/draw pair; those alone do not explain the
roughly 0.6 ms per physics tick cloudlet increase.

Water-draw sampled medians rise from 0.3441 to 0.6738 ms (default), and 0.3389 to
0.6676 ms (far off). Terrain rises from 1.2114 to 1.4899 and 1.2032 to 1.2472 ms.
Cloud marching changes much less (1.7060 → 1.7485 and 1.6968 → 1.7797 ms).
Shadow-cascade object-cull medians remain approximately 0.62–0.63 ms.

## Added attribution, pending installed measurement

Use the existing `POSEIDON_SIM_VEHICLE_COST=1` opt-in before starting the game.
The new diagnostic uses the existing report/reset cadence (approximately one
second) and emits five `Sim cloudlet cost over ... ticks` receipts. The default
off path adds no clock reads or accounting. Existing stage IDs and the original
Cloudlets container remain intact. Each receipt reports actual call count,
total ms, ms per existing physics tick and maximum individual call ms:

| Nested scope | Actual measured work |
| --- | --- |
| cloudlets.soft-surfaces | GSnow.Advance plus admitted GMud.Advance and GSand.Advance |
| cloudlets.runoff-source | Actual coarse domain configure/regrid/bed update and optional fine-source update |
| cloudlets.runoff-advance | Actual RainWaterField.Advance, including its periodic solver steps; stale-domain branch also measured |
| cloudlets.runoff-natural | Actual natural-source update and its associated source receipt |
| cloudlets.remaining-effects | Debug smoke admission and the remaining legacy cloudlet simulation/deletion loop |

They are nested attribution, not an exhaustive additive partition: small native
liquid-rain/domain predicates, solar evaluation and sea-level assignment fall
between these scopes. Source/natural receipts, if emitted, lie inside their
corresponding timers. An Advance call is recorded even when its accumulated
solver time has not yet reached the 0.25 s step; the max-call receipt can expose
periodic heavy calls without introducing per-frame logs or readbacks.

Focused source tests compile the actual native clock/accounting implementation
and check off/on gating in separate processes, rejected container indices,
total/count/max accounting, idempotent Stop/destructor and existing Reset.
`scripts/Test-CloudletCost.ps1` passes both modes. A token-order check and manual
scope review preserve original non-timing operations. The whole-engine build
and installed matched flight run remain required; no new runtime timing or
performance improvement is claimed here.

If actual Advance accounts for the increase, the next bounded candidate is
reusing its identical first-pass edge transfer values in the second pass.
`RainWaterField::Step` currently recomputes each transfer while bed and depth
remain unchanged between the two passes (roughly 525000 edges per pass on this
domain). A roughly 2 MiB raw-edge cache trades repeated square roots for memory
traffic; it requires measured justification and conservation/equivalence tests
before implementation. No solver, water quality, weather-map cache or map
admission policy is changed by this diagnostic.
