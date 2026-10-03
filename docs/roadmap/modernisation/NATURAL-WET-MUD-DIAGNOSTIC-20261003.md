# Natural wet mud diagnostic

The original-map controlled-player runner has two optional arguments:

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
