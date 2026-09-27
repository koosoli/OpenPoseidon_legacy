# Controlled original-MLOD offline producer — 27 September 2026

The optional Tools producer now turns an actual owned original P3DM snapshot into a selected CLOD disk package. No new original-file package is consumed by the game. Experimental flags remain OFF, and whole-plan completion and performance acceptance remain open.

## Executed checkpoint

Source 38765117 adds an optional offline original-P3DM producer, not a game consumer. Repaired all-five-target builds passed 435 and 13 scheduled steps; focused actual export/packing/producer checks passed 314 assertions in 14 cases without skips. Initial friend-access compile failure and live-TestApplication unit assumption failure remain historical. Actual input 24,136 bytes produced selected package 70,084 bytes, authored fallback 2/coarse 63/fine 512 triangles; invalid trailing byte was refused with exit 2 and no output. Supported clean installation at 17:35 passed disabled ordinary-MLOD smoke with exit 0, normal shutdown, no forced termination and two admitted red boxes; local inspected appearance showed no obvious change. It did not consume the new package. The controlled untextured two-visual-LOD subset retains assumed original winding. Owned input cap16 MiB and serialized cap128 KiB are logical bounds, not upstream CLOD scratch/time or RSS guarantees. Ordinary controlled=false conversion retains policy with a small false branch/null-context guard cost. No runtime/default paging, new live bake or world asset path is activated. Whole plan OPEN/defaultReady-performanceAccepted false. Details: CONTROLLED-MLOD-PRODUCER-20260927.md.

Actual raw identity is `745b2fab0ab7571bd743199480ec65fdbe956c2c4d44290abd67f8acdf41e89e`; selected derived identity is `5f80bf2dc2ea5766b20ecbb80a2e135026929e48e44ac2523bf027cf50d60319`. Whole selected-file SHA256 is `fd2fda62dc60a1af8c50cc697f89e4c11dcdcfebea974302825b105146702cd1`. The product contains one coarse and one fine page, seven selected clusters and 88,156 known source bytes. This authored source is controlled original-format evidence, not arbitrary retail compatibility.

## Input, context and trust boundaries

One owned snapshot (<=16 MiB) supplies the original-byte hash and the single direct loader call. Bounded stack preflight without input-sized table allocation rejects unknown tags/flags, material dependencies, nonfinite values, invalid references and trailing bytes. Independent parsed IR checks precede conversion: exactly two visual LODs, canonical autocenter=0, one untextured material, bounded geometry and complete source-face mapping. Original winding remains assumed.

Controlled conversion requires joined owner, non-reversed complete-tail mode and three empty bank tables. The friend implementation repeats these conditions before allocation, so a direct call cannot bypass isolation. Ordinary controlled=false conversion retains existing config/path/quality/tail behavior, with a new false-mode branch/null RAII context guard cost. No literal zero-footprint claim is made.

Pinned meshoptimizer is `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`; CLOD/RAM adapter versions are 1. Exact attributes and selected topology use existing bounded decoders and the <=128 KiB field-wise codec. Upstream bake is synchronous and uninterruptible; input/output ceilings do not hard-bound its scratch, time, allocator overhead or RSS. A new private directory holds the product and separate external producer manifest. Expected raw and derived keys must be independently established, rather than trusted from declarations inside the disk file.

## Historical failures

The first build failed because renaming the conversion implementation lost Shape/LODShape friend access. Exact declarations/friends and a repeated isolation predicate repair it. The first focused run passed six of seven cases and 94 of 95 assertions; only its assumed GApp==nullptr assertion failed because TestApplication remains live. The unit baseline now scopes GApp null only around ordinary comparison; the exporter is tested with the live application. The old shape-export tag matched nothing; final corrected geometry-page-export filtering actually ran. Failed logs remain evidence, not passing checks.

## Installed default smoke and remaining gates

The ordinary two-red-box fixture passed disabled smoke at1280x720, normal shutdown17:36:10.131/complete17:36:10.301 and forcedTermination=false. Root inspected its local opaque geometry without an obvious change. No original-file CLOD package was submitted by this smoke. Supported clean installation is38765117/17:35; Rust startup header retains doc HEAD911801eb from compilation. Exact installed EXE SHA256 is `4758A5DE877993D8E60B3461DC3A4337D3F8A11A39507DE16E515B1D84A5C57F`; DLL is `0A6B0305850C995F31CB15D89B42854242CB8A15B92F4A82AAE9E35F3CF27D95`.

This proves narrow ordinary-load compatibility, not route FPS, absence of regression, arbitrary asset fidelity, pixels/all passes or physical GPU reclaim. Actual new-file runtime consumption remains a later gate. Earlier useful-upload failures, ResourceGrowth and ownership limits are preserved.

## Artifact hashes

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `build/build-controlled-mlod-asset-20260927.log` | 41808 | `3f6a6eb472528d7eae1f54947f0a1c2e975e5f9dae78368c4d5028ac240e7ade` |
| `build/build-controlled-mlod-asset-repaired-20260927.log` | 161847 | `ea678d4a9c389d695dfda09085d92709fdad5d688a847902fb12f059e62f45cb` |
| `build/test-controlled-mlod-asset-20260927.log` | 461 | `959c029f5dab785290ed10779a47559f76e863ba92002735dd4aa13baf981a07` |
| `build/build-controlled-mlod-producer-20260927.log` | 25317 | `3160c266e56ca8e82ef82673aad25b6baf34d1b7836dffcca9c22132d1488e8a` |
| `build/test-controlled-mlod-producer-20260927.log` | 165 | `333d66d73881057fae678fc8b67d734e0c4270f766140b4659ef1e8c94653e50` |
| `build/tools-original-mlod-20260927.log` | 1584 | `510f9bfbac17d8bef3ee74abe41054f66c78c09b12c13a5dca98e94a07e02b8d` |
| `build/tools-original-mlod-invalid-tail-20260927.log` | 76 | `c1a2c8f7ace347d7dc5a217288836c41b301e37a02260c7a285e17370c7bda66` |
| `build/original-mlod-fixture-source-20260927.json` | 937 | `402795d35ea5b1ee340c2f8239cf029e9b2d95f360fb442922314e90c54c8bd2` |
| `build/original-mlod-cli-20260927/input.p3d` | 24136 | `745b2fab0ab7571bd743199480ec65fdbe956c2c4d44290abd67f8acdf41e89e` |
| `build/original-mlod-cli-20260927/invalid-tail.p3d` | 24137 | `5d9f5d882fd10cee1540de1482bf35e18ecdb1df27fa5c81224aea1e5a0d8f58` |
| `build/original-mlod-cli-20260927/valid-output/manifest.json` | 1581 | `67a83168beba59896f1450bdc7ef5ba72318170728dd49edc294e70f132be79b` |
| `build/original-mlod-cli-20260927/valid-output/selected.gcd` | 70084 | `fd2fda62dc60a1af8c50cc697f89e4c11dcdcfebea974302825b105146702cd1` |
| `build/stream-residency/original-mlod-default-20260927-20260927-173558-a43f62/result.json` | 8437 | `1f3593b7fbba9c299a30cd048e78f8dd4cfbed08803d03433dec27446dc34c31` |
| `build/stream-residency/original-mlod-default-20260927-20260927-173558-a43f62/provenance.json` | 5946 | `bbce68279fd310be7fc87c055db1dcda9d8170ff71cd61bec2558c85c8dd1434` |
| `build/stream-residency/original-mlod-default-20260927-20260927-173558-a43f62/engine.log` | 92355 | `0ae91a1efc49b48a8dd834999f5b9a07dab078844702d11d3e44ea3d8364f98d` |
| `build/stream-residency/original-mlod-default-20260927-20260927-173558-a43f62/harness.jsonl` | 7800 | `eab313adea431044c6c1a5e0d98456802d5a8f0710c7a85b3bc12b7ee76151cd` |
| `build/stream-residency/original-mlod-default-20260927-20260927-173558-a43f62/coverage.json` | 4017 | `dbfee836c22d1fa88fd742a293bdf09d1f86eb5b35eb14793ab0206ca566a4f7` |
| `build/stream-residency/original-mlod-default-20260927-20260927-173558-a43f62/fixture.png` | 1767152 | `bcfbd3ef78fe2cfb2017d1495f6046cc26f936b403f4194d4a10132d37023c53` |
