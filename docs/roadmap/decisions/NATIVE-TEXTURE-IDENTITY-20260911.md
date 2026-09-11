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

## Lazy BCR tint experiment (September 11, afternoon)

Candidate `WGR_LAZY_BCR_TINT=1`, off by default pending acceptance. With the
existing shader-tint mode and linear multiplier, a strictly BC7 `_bcr.edds` /
`_bcr.dds` source lends an immutable CPU view to its tinted variants. The same
TextureBank-owned inner texture already supplies their GPU image. The view
decodes/tints only the smallest mip for the exact average; requested CPU mip
pixels are still computed with the existing linear LUT, BGR order, BCR opaque
alpha rule and destination-format conversion. No decoded chain is retained.

The wrapper retains the inner Texture before the view is installed, and member
destruction releases the view before that owner. No extra cache/manager/worker,
mount read, GPU allocation or renderer-thread change. Unsupported formats,
composites and legacy mean-rescale use the old source factory. Changing to baked
GPU tint can still upload the view's pixels; it must not drop the borrowed owner.
Alpha classification uses the same inner image as the existing GPU delegate.

Unit comparisons cover all mips, exact bytes/averages, padded ARGB rows, packed
destinations, independent alpha flags, mip caps and refused inputs. Run the same
native route with `Test-NativeStreamCancellation -LazyBcrTint 0/1`, identical
profiling and driver-memory sampling. Tests and installed A/B are pending at
this entry. Saving CPU payloads is not itself evidence of improved frame times.

### Initial A/B and exact-average correction

Installed a11b8650, native 1280x720 MSAA4, identical 4 km route/settings/samplers:
ON `cancel-20260911-144633-...-lazy-bcr-1-...`, OFF `cancel-20260911-144841-...-lazy-bcr-0-...`.
Both strict lifecycle checks pass and register 22370 instances. Full-window moving
FrameProfiler: ON n=1316, median 29.729, p95 42.317, p99 83.792, max 142.632 ms,
4 frames >100 ms; OFF n=1283, median 30.342, p95 43.551, p99 91.549, max 218.512 ms,
6 frames >100 ms. Process private peak 7942717440 vs 11150643200 bytes (~2.99 GiB
saved). Both retain exactly 1417 object textures / 1003.057 MiB and 272.790 MiB
live geometry. Whole-GPU driver peaks 4372 vs 4354 MiB: not a VRAM saving.
One pair does not establish a repeatable FPS/tail improvement.

Village OFF 145233 / ON 145427, native 1920x1080 MSAA4, 90 s warmup: both pass,
22360 instances, 5362 main objects, 13499377 main triangles. Inspected wall/roof/
road/fence/sign colours and coverage agree; foliage/cloud positions vary.
Single GPU samples 29.999 / 30.087 ms are not evidence of a steady FPS gain.

Exclude the preceding 143510 run: the graphics.cfg path reached 262 characters;
the legacy reader missed it and activated auto DLSS. The harness now uses a short
profile path, rejects overlong profile paths, and asserts native dimensions in
both route and village captures. Merely specifying DlssMode 0 was not the fix:
it was already the helper default.

Expanded tests revealed that truncating the bank source to MAX_MIPMAPS=7 loses
the original smallest mip used for the average. The candidate then used a
different mip for its tinted average. Synthetic capped-source tests failed six
assertions; five real native BCR variants at caps 256/1024 failed nine average
assertions (mip pixel comparisons passed). Preserve that one compressed smallest
level when truncating; include its bytes in prepared-source/cache accounting.
No whole decoded chain is restored. Corrected suite: 1664 assertions, 66 passed
cases / one optional worker case skipped; real-corpus oracle: 449 assertions pass.
Worker-enabled suite also passes: 1671 assertions in 67 cases. Repeat installed
comparisons before default activation.
Reproduce the read-only corpus check with POSEIDON_TEST_REFORGER_ADDONS set to the
local addons directory and PoseidonTests '[lazy-tint-corpus]'. No proprietary
texture data is copied into the repository or package.

### Corrected repeat and default decision

Installed 8e495715, same native route in reverse order: OFF 151511, ON 151721.
Both pass lifecycle/coverage gates. Full moving windows: OFF n=1289, median
30.660, p95 43.465, p99 88.430, max 173.359 ms, 8 frames >100 ms; ON n=1286,
median 30.412, p95 43.342, p99 87.332, max 144.192 ms, 3 frames >100 ms.
Private peaks 11232161792 -> 7855013888 bytes (~3.15 GiB saved). Whole-GPU
peaks 4391 -> 4370 MiB fluctuate, not an attributable VRAM win. Both still
register 22370 instances and retain 1417 / 1003.057 MiB object textures and
272.790 MiB geometry. Alpha-scan time falls 864.726 -> 1.314 ms, but texture
create timing increases 3082.907 -> 5104.178 ms; do not add these overlapping
admission/upload counters into an invented total speedup.

Decision: enable the narrowly eligible view by default. The repeatable benefit
is avoiding ~3 GiB of CPU payload, with modest/variable frame-time benefit and
fewer >100 ms spikes on this route. No shader, resolution, sight distance, GPU
texture data or geometry reduction. WGR_LAZY_BCR_TINT=0 remains the old CPU
path for A/B. Unsupported materials still fall back. No claim that native
streaming, forest quality, world-switch acceptance or the full backlog is done.
Run an installed default (no override) village/route smoke after deployment;
the harness now distinguishes default from explicit on/off in its metadata.

### Default smoke lifecycle failure, not a performance result

Installed f6516c52, Village-lazy-default-20260911-152834: native 1920x1080,
MSAA4, no lazy-view override, positive lazy-source diagnostics and the expected
5362 main objects / 13499377 triangles. Screenshot appearance agrees. However,
shutdown stopped after DDTerm / sun / moon messages, without Shutdown complete;
the 300-second watchdog killed its own PID 43884. This is a FAILED smoke test.
The village wrapper incorrectly printed success despite failed capture metadata;
5782c318 now checks the subprocess exit and strict lifecycle metadata. Direct
helper checks reject this failed run and accept the earlier valid 145427 run.

Same installed f6516c52, same village: explicit OFF 154000 and ON 154151 both
pass the repaired lifecycle gate (including audio cleanup and Shutdown complete).
Both show 22360 instances / 5362 main objects / 13499377 triangles. Single GPU
samples 29.676 / 29.672 ms are not an FPS comparison. The prior hang is intermittent,
not root-caused by this ablation; do not claim the lazy view caused or fixed it.

Source inspection finds Engine::StopAll is a no-op for WGPU; StopRenderThread
currently runs in the renderer destructor after world teardown. That is an
ownership/lifecycle hypothesis, not a proven deadlock. No speculative shutdown
change has been made. Capture a stack from an owned hung test if it recurs;
keep default no-override lifecycle and world-switch acceptance open.

Follow-up installed 89030aee, Village-default-retry-1810: no override, positive
lazy-source diagnostics, strict native 1920x1080 MSAA4 and clean exit. The repaired
wrapper accepts lifecycle and 22360 / 5362 / 13499377 coverage. GPU endpoint
32.439 ms is not a new paired performance result. This closes the pending default
smoke, not the isolated prior hang or world-switch acceptance. Selected main LOD
histogram is all LOD0 in this pose; inspect thresholds/quality before inferring
that authored native LODs are absent or changing the sight distance.
