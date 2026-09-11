# Native texture identity and loading costs

## Diagnosis

Installed ea66561f run cancel-20260911-032933-slow-trace-dds-prep-bank-trace-bc7-default-driver-memory-tint-reuse-0
completes cleanly. Adapter bank instrumentation isolates load/header/material
costs without changing scheduling. Logged table calls (>=1 ms) total 10697.26 ms
loading, 0.35 ms headers, 2.09 ms materials; maximum texture-load component is
172.17 ms. Optimizing CfgTextureToMaterial would target the wrong cost here.

DDS records (>=2 ms only) contain 74 repeated canonical paths, 444.884 ms of
second-call work. The WGPU bank interns exact names, while the mounted archive
resolves case and separators equivalently. Different spellings unnecessarily
create separate source objects, decoded mip storage and GPU uploads.

## Implementation e1738ab4

EnfusionMount::CanonicalPath returns an identity only for an actual mounted
entry. TextureBankWgpu::Find first preserves exact matching, then checks that
archive identity. Load registers the canonical alias in the EXISTING bank.
Original Texture names remain intact. No second cache, extra workers, mip/LOD
reduction or change to GPU-thread ownership. ReleaseAllTextures clears aliases
with the bank. Loose files and synthetic tint/coverage names remain exact.
WGR_NATIVE_TEXTURE_KEYS=0 restores the previous lookup for A/B comparisons.
Default is on. PAK tests: two cases / 60 assertions, including separators/case,
missing/synthetic paths, close/remount and concurrent read lifetime.

## Installed comparison, same e1738ab4 binary

ON: cancel-20260911-033914-slow-trace-dds-prep-bank-trace-texture-keys-1-bc7-default-driver-memory-tint-reuse-0
OFF: cancel-20260911-034155-slow-trace-dds-prep-bank-trace-texture-keys-0-bc7-default-driver-memory-tint-reuse-0

Same native Everon 4 km / 100 m/s camera route, 1280x720 native (DLSS off), MSAA4,
version-3 Ultra profile, unlimited FPS, LOD governor range 1, same shadow settings,
same detailed logging and 1 Hz process/driver memory sampling. DDS worker and
tint decode reuse experiments remain off. Both strict lifecycle checks pass;
both register 22370 instances. Full-window FrameProfiler values, not log gaps:

| Moving | OFF | ON |
| --- | ---: | ---: |
| Completed frames | 1277 | 1293 |
| Median ms | 30.531 | 30.181 |
| p95 ms | 43.424 | 43.092 |
| p99 ms | 95.554 | 90.417 |
| Maximum ms | 260.249 | 211.333 |
| Frames >100 ms | 9 | 10 |
| Peak process private bytes | 11814268928 | 11209318400 |
| Peak whole-GPU driver MiB | 4885 | 4631 |
| Endpoint tracked object texture MiB | 1251.552 | 1003.057 |
| Endpoint object textures | 1754 | 1417 |
| Endpoint geometry live MiB | 272.790 | 272.790 |
| Repeated canonical paths in >=2 ms DDS records | 68 | 0 |

Evidence supports reduced duplicate work and memory: ~0.56 GiB lower process
peak and ~248.5 MiB less tracked object texture residency. Whole-GPU memory
includes other programs. One pair does NOT establish a stable FPS or tail win;
>100 ms frames do not uniformly improve. Preparation records are thresholded,
not complete call counts. The route endpoint is ocean, not visual acceptance
of the village.

Separate installed Village-keys-on-20260911 / Village-keys-off-20260911 captures
pass strict lifecycle checks with e1738ab4, 1920x1080 native MSAA4, isolated
version-3 Ultra profiles, fixed pose and 90 s warmup. Both show 22360 registered
instances, 5362 main objects, 13499377 main triangles and 612.630 MiB live
geometry. Inspected images preserve roads, walls, roofs and foliage; wind/cloud
positions are not pixel-identical. Tracked object textures fall from 1601 /
1163.141 MiB to 1291 / 932.521 MiB. Captured frame times 30.375 OFF vs 30.447 ON
are effectively unchanged, not evidence of a steady-pose speedup. These are
targeted village checks, not all-map/interior visual acceptance.

## Next bottleneck

Large unique native texture reads/decompression/composition still block initial
bank loads. The shader-tint path in TextureWgpu::EnsureUploaded delegates GPU
uploads to the shared inner image, but Init still prepares baked CPU sources for
the synthetic names. Investigate retained CPU data needs (average colour, alpha,
fallback/direct path, live tint-mode changes) before trying lazy composition.
Do not simply drop sources or reuse another tint's pixels. The earlier blanket
DDS worker and tint-decode-cache experiments had no stable measured gain and
remain off. This increment does not declare native Reforger stutter solved.
