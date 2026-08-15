# RND-038 — Four worlds, four bottlenecks, measured at play resolution

**Date:** 2026-08-15
**Branch:** `feat/texture-streaming`
**Status:** MEASURED. All figures at **1600×900**, settled (`pending=0`), unless stated.
**Related:** [RND-036](RND-036-texture-mip-upload-cost-one-vfs-open-per-level-20260815.md),
[RND-037](RND-037-pass1-is-94-percent-submission-and-it-is-linear-20260815.md)

## First, a correction that invalidated a headline

Every framerate in RND-035/036/037 and in this session's commits up to `017cd22` was
measured at **800×600**, because that is `farfield-bench.ps1`'s default and nobody
overrode it. The owner plays at **1600×900** — 3× the pixels. On a GPU-bound world the
GPU frame scales close to linearly with pixel count.

| Reforger Everon, settled | 800×600 | **1600×900** |
|---|---|---|
| new default (governor 4 + retained foliage) | ~50 fps | **40.5 fps** |
| legacy (governor 16, bail on) | ~30 fps | **25.6 fps** |

The relative win survives (+66% → **+58.4%**); the absolute number does not. "~50 fps"
was reported as a play-resolution figure and was not one. **Quote the resolution beside
every fps number, and use 800×600 only for comparing arms with each other.**

`--dev` costs ~1.5 fps (40.5 → 39.0) and is not worth controlling for.

## The four bottlenecks

| world | fps | CPU frame | GPU frame | dominant cost | status |
|---|---|---|---|---|---|
| Reforger Everon | **40.5** | 24.7 ms | 23.1 ms | GPU geometry | fixed (+58%) |
| Arma 3 Stratis | — | — | — | Pass1 CPU draw | fixed (+159% @ 800×600) |
| DayZ Chernarus | 31.3 | 31.9 ms | 13.2 ms | **texture VRAM** | renders now; pin model still wrong |
| **Arma 1 Sahrani** | **20.17** | **49.6 ms** | 20.9 ms | **`drw:land` 35.8 ms** | **untouched** |

Every one of the four contradicted the generalisation from the world measured before it.

## Arma 1 Sahrani — the one nothing has addressed

Measured for the first time this session. The world had never been exported; `sara.wrp`
lives inside `packages/a1-compat/addons/sara.pbo` (63 MB, OPRW rev 20) and had to be
extracted before it could be benchmarked at all — see the recipe below.

```
avg fps 20.17 | CPU 49.584 ms | GPU 20.868 ms
drw:land 35.839 | swap 8.905 | setup 2.465 | drw:prep 1.584 | land:gnd 0.445
PERF pass1: objs/frame=780 submit=0.054 ms | coverage full=33815 (97.3%) partial=0 none=938
```

**`drw:land` is 35.8 ms — 72% of the CPU frame.** That phase is grass, alpha objects
(Pass2) and the segment tail. It is not `land:obj`: Pass1 costs 0.054 ms here, and the
coverage counter shows **97.3% of objects take the GPU divert**, so the foliage and
registration work already does its job on Arma 1 content. Nothing in this session touched
`drw:land`.

The owner's report — "Arma 1 was bad no matter where I went, Arma 3 runs better" — is
confirmed and now has a cause.

**Next step is the one that already worked twice:** split `drw:land` the way
`WGR_PASS1_STATS` split `land:obj`. It is one phase name hiding at least three different
costs, and 35.8 ms of a 49.6 ms frame is the largest single unexplained number left.

## The streaming transient is what the owner actually sees

Everon needs **107 s at 1600×900** to reach `pending=0` (81 s at 800×600), and frames run
200 ms+ throughout. "I launched it and it was under 10 fps" is a report about the fill-in,
not the settled frame — both are true and they are different problems. Chernarus is 253 s.
At ~1.1 ms of CPU per object created this is unattributed within itself; the texture side
is ~3% of it (RND-036), so the remainder is the synchronous ODOL parse and object
construction.

## Recipe: extracting an Arma 1 world

`sara.wrp` is stored uncompressed inside `sara.pbo`. A PBO's header starts with a `Vers`
properties block (`method == 0x56657273`) followed by NUL-terminated key/value pairs — skip
it or the entry table desyncs and every offset is wrong. Entry: NUL-terminated name, then
5 × `uint32` (method, originalSize, reserved, timestamp, dataSize); the table ends at an
empty name with `dataSize == 0`; payloads follow in table order from that point.

Benchmark it with `-AddonRoot packages\a1-compat\addons` (a mod folder would be rejected —
they are not interchangeable). Confirm `registered instances` against the placement count:
193,645 here.
