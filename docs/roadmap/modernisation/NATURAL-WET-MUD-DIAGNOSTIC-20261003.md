# Natural wet mud diagnostic

The original-map controlled-player runner has optional arguments:

- -MinimumMudWetness .8 waits for actual GMud wetness at least .8 before the
  wet walk. Its allowed range is .3 to .95; the default remains .3.
- -TerrainWetnessTrace enables the existing WGR_TERRAIN_PUDDLE_FIXTURE=1
  diagnostic. It records queued terrain weather at the wet ready point and
  actual mud contact/source-centre views. It does not pin wetness or rain.

A root-owned installed run can use these alongside -Maps noe and its required
-ExpectedCommit and serial game lock. -WetDeadlineSeconds 120 allows a longer
bounded natural wait than the unchanged default 75 wall seconds. The existing
setAccTime 4 advances actual simulation during that wait. No manual deposit,
wetness override, contact stamp, terrain edit or synthetic rain history is added.
The last actual wait state and requested threshold are retained on failure.
If ordinary weather cannot attain the requested threshold before the deadline,
the campaign fails instead of lowering its threshold or manufacturing wetness.

The cloth and actual liquid-rain thresholds remain .3. The higher mud threshold
applies only where the original source census admits mud. Unsupported maps
retain their explicit soft-source gap and ordinary cloth/body controls.
A higher physical wetness threshold does not establish a decimetre depression;
the actual walking/contact samples and images still determine the relief.

## What the telemetry means

GMud::Wetness controls physical contact capacity and geometry. It is independent
of the terrain coat's TerrainPuddles::Wetness history. TerrainWgpu obtains the
latter from the captured liquid weather, solar exposure and wind, then queues
it in _pad1. The diagnostic runs before QueueTerrainParams and prints this
queued value every five simulation seconds. The parser exposes it as
queuedPad1 rather than claiming shader/GPU acceptance. Its printed rain field
is queuedRainStrength: the terrain master-off control zeroes that field before
logging. The separately queried actualLiquidRain remains explicit.

Terrain.wgsl derives the coat from tp.rain_wetness, source material admission
and snow exclusion. It then multiplies by physical interior rain reach and
coverage. These local per-fragment gates are absent from the existing CPU
diagnostic. Therefore an accumulated/queued value does not prove that a given
contact pixel receives wet coat, that Rust accepted the parameter, or that a
GPU frame completed. The receipt records the observed clock minus the latest
logged clock without claiming same-frame identity or fresh pixel proof.

The diagnostic environment switch also logs a candidate flat site at load;
that candidate does not select these captures. This runner keeps its original
source-census site and actual Field query. It records the inherited
WGR_TERRAIN_PUDDLES control, clears the existing wetness override, and restores
its changed environment on exit. Master-disabled zero upload is retained
truthfully, rather than rewritten as a wet input.

## Captures and checks

On admitted mud, the optional trace produces low/top images of a real walked
compressed cell and the source centre at its actual sampled physical height.
The centre can remain untouched and is labeled separately from the contact.
Existing source tuples, height/gradient stability, paused player/clock/Person
wetness/physical stores/weather, body draw, cloth upload and lifecycle gates
remain. Captures occur after the ordinary movement tests while paused;
they do not pin a moving player or change its contact geometry.

The original runner -SelfTest covers default/high natural thresholds, unsupported
source handling, liquid-rain exclusion, malformed thresholds and strict CRLF
telemetry grammar. Queued master-disabled zeros remain distinct from accumulated
history. Source checks alone do not accept installed appearance, local rain
coverage, deeper mud, GPU publication or performance.

## Paused wet-soil lighting comparison

-MudLightingViews adds eight captures: low/top views of the actual most
compressed walked mud sample and the admitted source centre, first at overcast
1 and then at overcast 0. Both use the owned mission's 1985-06-21 16:00 daylight.
This is deliberately controlled lighting of retained rain-wet soil, rather than
an observation of unforced natural cloud development. The option defaults off;
-TerrainWetnessTrace keeps its existing capture recipe.

Before walking, this option waits for actual GMud wetness >=.8 and the existing
terrain diagnostic's accumulated and queuedPad1 values >=.8001. The latter
margin accounts for the log's four-decimal rounding: a printed .8000 can
represent .79996. A requested -MinimumMudWetness above .8 still applies.
Use -WetDeadlineSeconds 120 for a longer bounded rain wait if needed. The coat's
master-off zero cannot satisfy the comparison, and the inherited wetness override
is cleared as in the ordinary runner. The existing diagnostic is enabled before
launch so its naturally accumulated ready receipt precedes the paused views.
This receipt proves the existing C++ queue input, not Rust/GPU acceptance or
per-fragment rain reach and coverage.

Every view retains the actual source tuple, sampled relief and gradients,
simulation clock, player position, Person wetness, physical store state and
actual liquid rain. It checks actual sun availability, negative light-travel
Y for a sun above the horizon, daytime nightEffect, requested overcast and the
shared solar exposure expression. The clear view needs positive solar exposure;
the fully overcast view needs zero direct exposure. Terrain receipts are recorded
alongside those actual lighting fields, and the paused diagnostic row must stay
unchanged. No manual mud deposition, forced rain history, animation or source
geometry is introduced.

Finally, the comparison restores its original overcast and the known owned
calendar date. Since the engine has no date getter and setDate accepts integer
minutes, the runner bounds the original time to its same-date 16:00–17:00
interval, restores that minute, then uses registered skipTime to recover the
reported original dayTime with at most three corrections. Exact dayTime and
overcast readback are required, together with the paused state guards. Source
self-tests cover the recipe, rounded/disabled/unaccumulated wetness rejection,
actual sunlight falsifiers and bounded date restoration. Installed runtime and
visual acceptance remain a separate root-owned check.

## Rendered cloud and actual material limits

The e017 / c697d3 installed campaign passed the paused contact/source and date
checks with eight mud views. Its ready C++ diagnostic had accumulated/queuedPad1
.8083, and the actual contact and centre both reported `eden\\pol.paa`,
Field/pol/dirt with no character. It retained readable relief, but the low views
were visually matte and near-identical under the two requested cloud values.
This does not accept wet-soil appearance.

The original recipe forced `WGR_CLOUD_COVERAGE=0`. EngineWgpu applies that pin
after the ordinary overcast-to-rendered-coverage drive, and its resulting
`_sky.cloudCoverage` also controls directional beam dimming and ambient boost.
Simulation overcast/solar checks therefore did not establish a rendered-cloud
comparison. Either optional quality-lighting switch now clears the process
coverage pin before launch, records its inherited value, and restores it on exit.
The default contact runner retains its existing clear pin. Rendering still
depends on actual sky enable/weather-drive settings; an unpinned environment or
simulation cloud readback alone is not proof of packed/GPU cloud fields or pixels.

For the actual plain `pol` tile, TextureBank assigns the same resolved surface
to all four quadrants. TerrainWgpu consequently admits the exact Field/pol/dirt
source into SoftSoil bit 1 and also GroundReceiver bit 0. The legacy shader
turns that source bit into mud fraction 1; actual cell cross-fading can blend it
with neighbors at boundaries. This is a source-derived admission conclusion,
not a GPU material readback. The soil coat still multiplies by physical
`interior_rain_reach` / `interior_rain_coverage` admission. The captured queue
receipt cannot establish that local fragment gate. Broad shader roughness or
gloss changes should not substitute for evidence that these actual inputs reach
the tested contact pixels.
