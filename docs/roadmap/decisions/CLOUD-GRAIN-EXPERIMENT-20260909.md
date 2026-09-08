# Cloud grain: paired-density experiment rejected

Test build f33101fd, both safety refs pushed before matched EXE/DLL deployment.
Foreign Cargo.lock unchanged. Snow remains implemented and disabled by default.

## Experiment and measurements

Added one opposite-half-step density sample to each cumulus primary march step,
averaging density without doubling the sun/moon light marches. Reflection and
cirrus paths stayed unchanged. Switch WGR_CLOUD_DENSITY_PAIR was off by default.
The composed sky WGSL validated offline and the native build passed.

Installed 1280x720, DLSS Quality 858x482, RTX 3070. Last seven main cloud-march
GPU samples, median milliseconds (the new Cloud GPU ms log separates march from
composite; these are not reflection timings):

| Nogova scene | Original | Density pair |
|---|---:|---:|
| 8000 m, pitch -45, weather 0.25 | 0.565 | 0.991 |
| 4000 m, pitch -15, weather 0.65 | 0.926 | 1.358 |

Both use x=7764.82, north=4408.71, heading 20. Private captures/logs are
`.tmp-bulb/cloud-pair-{off,on}` and `cloud-dense-{off,on}`; the dense fixture is
`.tmp-nogova/cloud-pair.noe`. No convincing visible grain reduction compensates
for the approximately 0.43 ms cost. Remove the experiment rather than change the
owner's default cloud look. Retain separate cloud GPU timing for later diagnosis.

`cloud-dense-native` repeats the dense scene with WGR_DLSS=0: visible cloud grain
remains. All five processes exited 0. This is a STATIC comparison, not evidence
that the owner's motion-trailing report is or is not caused by DLSS.

## History-mask integration finding

The current bridge sends its engine history mask to TransparencyMask. The SDK's
Vulkan helper labels this field unused/reserved. There is a separate
BiasCurrentColorMask field, but switching to it is NOT a justified default fix:
the current DLSS programming guide, section 3.15, advises against this buffer
for current models and says only legacy preset F supports it. Do not change the
model preset just to make a mask appear effective.

Primary references, also verified in the local `.tmp-dlss-sdk` dependency:
- [Vulkan helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_vk.h)
- [Programming guide, section 3.15](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)

Consequently the older successful R8 mask readbacks only prove engine mask
generation, not that DLSS acts on it. Cloud motion vectors and representative
cloud depth are still absent. No cloud-trailing fix is claimed here.

94101879 removes the density-pair experiment and is deployed. Final native build,
sky WGSL validation, renderer-ledger validation and a repeat snow runtime smoke
test pass. No new default GPU sampling cost remains from this rejected experiment.
