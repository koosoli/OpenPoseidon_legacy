# Actual wet-soil fragment classification

The natural-cloud 3e388c Everon campaign passed the clock, source, real wetting,
camera and lighting controls, but its sunny soil remained visually matte.
The overcast low view contained a mirrored bush silhouette and a blue arc.
Terrain's ground_sky_reflection samples only atmospheric radiance; retained
rainwater separately samples scene reflections. Thus the mirrored bush region
cannot establish that pure-soil terrain accidentally retained its cosmetic film.
It also does not establish local soil rain admission beneath the overlay.

The opt-in diagnostic uses existing reserved bytes, without expanding public
renderer structures: WgrTerrainParams._pad3 / TerrainParams._pad3 at offset80
in the unchanged 88-byte record, then TerrainShadowMap._pad2 / pad_c at offset44
in its unchanged 64-byte record. All existing initializers remain zero. The
actual mapping is assembled and uploaded each main frame independently of
shadow-mask regeneration/enabled state. ground_weather.w remains actual time
and is not repurposed.

The appended Engine virtual has an unavailable fallback for other backends.
EngineWgpu requires its main-thread owner, a live renderer, HDR, a complete
previous terrain input and exact WGR_WET_SOIL_DIAGNOSTIC=1. Its normal mode is
zero; the harness accepts only integer setters0..4 and separate read-only state.
Negative, fractional, out-of-range and malformed setters are refused. The Rust
mapping also rejects nonfinite/non-enum padding to mode0. A setter requeues the
complete last producer terrain input with only the reserved mode changed:
pending queues reset after publication, so a padding-only partial record would
be incorrect. Neither the setter nor readers touch an in-flight worker slot.
Returned mode is producer queue state, not a GPU-completion receipt.

## Colour modes

| Mode | Terrain RGB before postprocessing |
| --- | --- |
| 0 | Unchanged ordinary shading |
| 1 | Actual source soil fraction; initial soil wet amount after source/snow, before shelter; queued terrain wetness |
| 2 | Actual local coverage; vertical reach; rain-proof factor, when the ordinary shader evaluated that query |
| 3 | Final rain-admitted wet soil; remaining cosmetic film after soil partition; procedural film before shelter/partition |
| 4 | Neutral dark terrain; independent retained-water overlay classification |

In modes1..4, every actually surviving retained-water fragment draws magenta
with its original calculated optics alpha. All ordinary water front-face,
enabled/source, supported local depth, sea-level, shelter and snow discard
guards remain. Magenta replaces only post-optics output colour, not admission,
depth, normal, source, alpha or blend state. It can obscure terrain channels;
an obscured pixel does not reveal the underlying soil gate. Mode2 outputs zeros
when the ordinary terrain path had no wet soil/film to query, which must not be
interpreted as a measured closed physical map. Tone mapping, exposure and later
postprocessing remain ordinary, so screenshot bytes are not numeric input
readbacks. These diagnostic images classify actual shader inputs and survivors,
not accepted appearance or a quantitative per-pixel GPU buffer receipt.

Terrain modes reuse actual computed source/initial wetness and actual production
rain locals after their source/snow/physical gates. The final-soil and film mode
uses the real soil_wet_surface result. No material flags, wetness overrides,
contact stamps, map geometry, roof witness or invented gloss is introduced.
Mode0 follows the natural shader path with its existing energy/alpha response.

## Installed recipe owned by root

Use the original runner's required serial lock and ExpectedCommit with
`-Maps eden -WetDeadlineSeconds 120 -WetSoilFragmentViews`. The option implies
MudLightingViews and its ordinary real-rain wetness gates, clears the cloud pin
for weather-driven CPU settings, and enables only the diagnostic process opt-in.
Defaults remain off. The four existing low views (walked contact and source
centre, overcast and clear) each add modes1..4 and a mode0 normal-return PNG,
for twenty additional captures. The original normal low/top images remain.

Each mode records the actual scene camera position/direction/up from the owner
thread and requires exact retention. Simulation time, Person wetness, player
position, physical stores, actual rain, source tuple, height and gradients stay
fixed; actual sun/cloud and matching producer sky recipe guards remain. Existing
stock body queue/draw evidence and terrain wetness receipts are recorded. Actual
invalid enum requests must fail without changing mode or camera. A finally block
resets mode0 and captures the normal return, including source/clock/camera checks.

Source checks: the runner SelfTest covers the bounded mode recipe and moved-camera
falsifier. Test-WetSoilFragmentShader uses existing matching naga/naga_oil libraries
to compose complete actual terrain, retained rainwater, direct and GPU-driven
shaders, verify both actual WGSL byte spans/offsets, and exercise the production
Rust enum refusal/reset helper. It builds no Cargo dependency or game and opens
no device. C++/Rust layout assertions preserve the same offsets in the full build.
Full compilation, installed mode captures, normal reset and visual diagnosis
remain root-owned; source composition alone supplies no runtime acceptance.
