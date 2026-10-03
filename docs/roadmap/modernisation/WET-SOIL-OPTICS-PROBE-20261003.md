# Bounded wet-soil optics probes

This slice adds diagnostics and tests only. It changes no water support, source
qualification, reflection colour, weather, mud/sand physics or renderer debug
API. Root owns actual device and installed-game runs.

## Sky mechanism, real GPU pipeline

Run the `sky::reflection_probe_tests` Rust test filter with `--nocapture` and
`--test-threads=1`. The GPU test constructs actual Sky resources, uploads the
existing uniform and renders the actual production `fs_sky_env` pipeline into a
separate 256x128 HDR readback target using its real bind group. Six texel centres
sample upward, centre-contact-angle, above/below horizon and steep downward rays.
The original env budget is compared with a test-only source clone changing only
its exact 12/2 cloud-step call to 128/4. A second source clone returns actual ray
y, deck transmittance and actual cloud interval endpoints in Rgba32Float.

The controls use the recorded contact world camera/altitude and solar elevation,
coverage 0/.95, otherwise documented WgrSky defaults and no horizon haze. They
exercise the current production algorithm and real GPU resources, **not the
installed frame's entire SkyUniform or its unknown cloud drift/look state**.
Readback is raw linear HDR, before exposure/tone mapping. Cloud-free budget parity,
actual UV/ray mapping, finite radiance, interval rejection and transmittance bounds
are asserted. No expected blue/grey RGB or aesthetic acceptance is invented.
`WET_SOIL_ENV_PROBE` rows carry this scope explicitly.

## Actual paused contact-edge receipt

Add `-WetSoilReflectionEdgeReads` to the existing installed original-weather
player runner's normal root invocation. It implies the existing fragment views
and their weather/source controls. Exactly `contact-overcast-low` receives an
extra 17x17 grid with .125m spacing over 2x2m around the real contact point.
Queries use the existing mud/water owners while simulation is paused. Each
coordinate is explicitly converted to the owners' float precision before sending.
The 289-row read has a 180s bound and retains current water/source identity,
physical history, camera, player, paused clock, weather and mass-budget gates.
Hard/unknown source refusals remain in the receipt. No stamp, texture, actor,
weather, map or water-depth override is added. Clear/bare/centre views and their
normal screenshots remain otherwise unchanged.

Then export:

```powershell
./scripts/Export-WetSoilOpticsProbe.ps1 -ResultJson <completed-result.json> -Map eden -OutputCsv <absolute-edge.csv>
```

The exporter requires the existing complete functional result status and normal
exit, all 289 unique ordered requested/actual points, a common source/water
identity and budget, and finite supported depth/slopes. It stores a provenance
sidecar with result/CSV SHA256, installed files, actual camera, states and the
logged queued wetness observation. A failed or incomplete result is refused.

Set `WGR_WET_SOIL_EDGE_RECEIPT` to that CSV and run only
`rain_water::edge_optics_tests::actual_gpu_recorded_edge_direction_and_retained_interface_eligibility`
with `--ignored --nocapture --test-threads=1`. It reads actual owner geometry and
replays the production reflection expression and imported wet-soil/standing-depth
helpers on the GPU. Independent CPU reflection comparison and <=8mm standing/SSR
invocation refusals remain assertions. At most 16 downward wet rows plus the centre
and a summary are logged. No real SSR-hit success is inferred from eligibility.

The owner slopes are **not actual pixel-footprint-filtered fragment normals**;
sourceEligible is actual CPU source admission, not uploaded material-table bits;
queued wetness is an existing producer observation, not same-frame GPU acceptance.
These limits appear in exporter metadata and GPU probe rows. Paired existing
fragment modes can locate terrain coat versus actual surviving water, but their
postprocessed pixels cannot be decoded as numeric channels. If these bounded
probes cannot establish the cyan-rim cause, a narrowly scoped actual-frame env or
filtered-fragment readback still needs review; no production Sky logging was added.

## Source checks completed in isolated worktree

- Existing original-weather runner SelfTest passes, including bounded grid count,
  unique coordinates and invalid-centre refusal.
- New sky control Naga parse/validation and half-radiance decoder tests pass 2/2.
- Exact edge probe composition/source-contract/CSV refusal test passes.
- Exporter synthetic policy checks pass for valid shape and refusal of failed
  lifecycle result or duplicate/wrong coordinates. These synthetic files are
  explicitly labelled unit fixtures and are not runtime evidence.
- Diff check passes. Actual full Rust test compilation, device probes, installed
  edge receipt and appearance assessment remain root work.
