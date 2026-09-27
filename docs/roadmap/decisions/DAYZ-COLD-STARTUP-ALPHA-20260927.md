# Actual DayZ startup operation-local alpha reuse

Actual original DayZ startup now provides a narrow positive witness for the existing optional cold texture handoff: six accepted operation-local alpha classifications during initial admission, plus a separate prefix of successful source-validated owned-chain uploads. The installed renderer/engine pair remains `dbe68b03`19:21; these checks change only the runner validation interval. The feature remains off by default and this result does not establish a performance gain, route reuse, or whole-model readiness.

## Validation intervals and retained failures

`-RequireColdAlphaPeek` continues to require a positive settled-baseline-to-returned route counter delta and an actual successful owned-chain upload. New `-RequireColdStartupAlphaPeek` is additive and mutually exclusive with that route requirement; it observes the initial pending admission snapshot through settled baseline and requires a positive initial-admission delta plus a separate actual upload witness. It does not redefine the route result or infer success from a flag alone.

The first attempt `build/stream-residency/dayz-cold-owned-alpha-20260927-disabled-20260927-194013` FAILED: the runner omitted the expected archive-binding flag even though the cold path legitimately enabled archive capture. Its finally cleanup forced only the owned test process, without a normal shutdown marker; no result file was produced. The completed provenance, samples, harness and engine logs remain evidence of this failed attempt, not a clean runtime proof.

The repaired route attempt `build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414` also FAILED `NotExercised` after a normal engine exit0 and shutdown complete19:45:30.485. Its first sample showed peeks0/resident0/pendingtrue; the settled baseline showed peeks6/resident19,923/pendingfalse; returned remained6. The route delta was0, `positive=false` and `requirementSatisfied=false`. Six genuine startup uses and16 valid truncated upload observations do not satisfy the requested route gate. Neither failed mode is relabelled as a pass.

## Completed startup and OFF controls

Startup ON `build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430` passed its actual startup requirement, with707 captured route frames/drop0. The first observed snapshot at17:54:48.0988318Z had peeks0/resident0; settled baseline at17:55:03.9915441Z had peeks6/resident19,923. `initialAdmissionPeekDelta=6`, `positive=true`. Returned remains6; the independent route proof still explicitly records delta0/positivefalse. The16 successful upload observations are capped and followed by a truncation marker. They name original DayZ plants/structures/signs PAA sources, not synthetic fixture textures. Root-owned process exited0, with normal shutdown complete19:55:44.025.

OFF `build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215` passed with784 frames/drop0, peeks0 in every sampled camera cut and no cold upload rows. It started before c822b64a was committed, so provenance correctly records runner/source HEAD ab8f53ec rather than retroactively claiming the later startup runner. Both runs used the identical installed EXE `9924A3E0B621A9D7FBAD70385774244B02D0E5987AA93DA166640FEED3CEAC9C` and DLL `00498E5AE4E71026C3E391415911AC0284D994D7AB20A22959134AD76B2A6A8E`. OFF shutdown completed19:53:30.410. The exact c822b64a script bytes are retained under `build/dayz-cold-startup-source-c822b64a` independently of future working-tree edits.

The six scalar counter increments and16 source-validated successful upload observations are independent witnesses and need not identify the same texture. They prove actual initial-admission use and actual owned upload, not a per-asset paired theorem, complete source family, all textures/material stages, GPU completion, pixel parity, or whole-model readiness. Cold read/source16MiB, selected-chain16MiB, seven source-level limit and ready128MiB logical caps remain unchanged. The existing top-mip borrow leaves the chain for its later exactly-once upload and preserves source/header/token checks and ordinary-reader fallback. This checkpoint adds no production decoder/cache/classification policy or default activation.

## Route screening and allocated work

| Arm | Frames | Mean ms | Usual median ms | Nearest-rank p95 ms | p99 ms | Max ms |
|---|---:|---:|---:|---:|---:|---:|
| Startup ON | 707 | 50.215958982 | 24.943 | 163.67 | 473.561 | 1187.379 |
| OFF | 784 | 44.682094388 | 24.189 | 141.212 | 350.133 | 875.841 |

Percentiles use sorted index `ceil(n*p)-1`; usual even-count median averages the middle pair. Both returned19,923/required0/pendingfalse, but ON geometry live716,514,504 and textures2,809,208,052/count2,530 differ from OFF711,584,488 and2,763,029,844/count2,502. Geometry capacity1,275,068,416 is unchanged; backend allocation differs5,360,765,056 versus5,315,569,168. The ordered runs have cache/asynchronous admission and unequal work confounds; ON also exercises archive source service and bounded diagnostics. ON has worse measured mean, median, p95, p99 and maximum than OFF in this pair. These figures cannot establish causal FPS benefit, no-regression acceptance, or a normal-default performance result.

Root viewed both returned images: the same local bus/kiosk/building scene had no obvious new missing geometry. This local still-image judgement is not pixel identity, all-pass/material acceptance, retail-world completeness, or a performance gate. No further runtime is required for this narrow validation interval; the next parked diagnostic is still pending and excluded.

The entire plan remains OPEN, `defaultReady=false` and `performanceAccepted=false`. Experimental cold handoff stays OFF. The failed route gate, earlier failed archive setup, bounded counters and observations remain explicit rather than being hidden by this separate startup success.

## Completed artifact manifest

| Artifact | Bytes | SHA256 |
|---|---:|---|
| build/stream-residency/dayz-cold-owned-alpha-20260927-disabled-20260927-194013/provenance.json | 3502 | `3856CBC70808B01F61F589A55D64909F37307355AB04C1178C4434A0102A3FD9` |
| build/stream-residency/dayz-cold-owned-alpha-20260927-disabled-20260927-194013/engine.log | 3210140 | `731008D9ED78151CFE646E9F07BE4D3D607FF8C44A76D71C17813C423782A22B` |
| build/stream-residency/dayz-cold-owned-alpha-20260927-disabled-20260927-194013/harness.jsonl | 71925 | `5D44340CE34DE2B1EC62CD22C85D6B87EE92B9FC365C1A7C732A714F4C95480F` |
| build/stream-residency/dayz-cold-owned-alpha-20260927-disabled-20260927-194013/samples.jsonl | 72009 | `8ABF583B54A2A2AE49A77E2FF5D3A9C315EB35BAC9F2FDB7844A69D3CB1F0CC1` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/result.json | 81834 | `77134EF102EEB82462CB9E78D7BDE0931498F6A7CB147EC36CDE6D66F96A0BC5` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/provenance.json | 3552 | `AE75239D337078603169B58E36F808E3BE576616CB4AD8922CA46A57719066B9` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/engine.log | 3203730 | `12C0895623BEC02542B5DE73457E8E012E49EAB5B37D047F296B62891B3CBAB6` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/harness.jsonl | 75948 | `3501AA8FF725CD740EBC37B0BE5AB67AD2E89B67174212466FDB02E60451BF81` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/samples.jsonl | 75616 | `318D32CDB9FD3642946C61E14B18228F41B26D8B46AC805643A7DB9ACAC768D9` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/route-frame-ms.json | 7704 | `A3506354C1E805F0867092E8E2F3E58690C4CD549CCE57055812859A8C1C350A` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/cold-owned-alpha-proof.json | 4227 | `BB7799D95927EB6CEA7CE4CBE7F5CC2D70A3E9122B1EA14D758F08D19631970A` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/archive-source-binding-proof.json | 482 | `B0AB7CC4FCBA9CAEF026451D90E4B53B313D0DD6CDF1148D775680AB242AA800` |
| build/stream-residency/dayz-cold-owned-alpha-fixed-20260927-disabled-20260927-194414/returned-settled.png | 2161965 | `2D8107B0BE352AD89A672FFB8C27910690484A2B28DBF538566FBADD12EA3822` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/result.json | 84238 | `1EE8837259B7F502BC03D9FA07F9A1FDFDF2684979F6E02B53EF491953C2E62C` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/provenance.json | 3592 | `A9ECB02009586588AB39459B2B1C19A3F9128E547D1F8C0C6093D63498AFC95E` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/engine.log | 3224246 | `F90848F14C632801F68DE67C90F40D818889E9C5C036CE24C5752BE711D13012` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/harness.jsonl | 72452 | `8A096AC4EFF7A795A7522B0E14370207C8A19C27EA7C079958059B668F076240` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/samples.jsonl | 72027 | `7458F9CB6EC3E79B93A73A17EC6A2790591EC7C21D3A35609EE21ECABBE166CE` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/route-frame-ms.json | 7742 | `68119FD55C611086D9C33860658892E6693200E405F1890268A05BD5B32D89BF` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/cold-owned-alpha-proof.json | 4227 | `BBA88E9BC3F0358D9447C9C75B4730BF4823E7928C12F672C6E457B4313AE8CD` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/cold-owned-alpha-startup-proof.json | 637 | `24ACB7F22D2536610675AACB536BCA9F965282F1FE1154861D34611863C98168` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/archive-source-binding-proof.json | 482 | `FD7914FFBC3132EE5BCB0950E079B19D722B1854D9F5051D2E148854436BADBF` |
| build/stream-residency/dayz-cold-alpha-startup-on-20260927-disabled-20260927-195430/returned-settled.png | 2163349 | `A60028DAC8552500F1FC2901327C8DA40D7D7F0B92577C563B5070C789DD8BCC` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/result.json | 78331 | `E1EE5CDC9A24DA9A1DE218466FC12A36CAE8043AE90B6AE69374EF68D0439A9D` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/provenance.json | 3510 | `1A4F42E4262339D1754815CD8759DB7F6389C10B7FB905922D10AB11AEF82B18` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/engine.log | 3209396 | `8FE0266551FE27575B782EBD0140C4E82D8698B697D55E663ADC8C20A8FC4E0A` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/harness.jsonl | 72332 | `25ABB6E78CCAFD5BB2EBED8DB27450BEB07508B779EADB7DAD31AB13ACBD554A` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/samples.jsonl | 71918 | `3E814C1671FCE25E29925E3D0FDE9C0FBBC11B6A374CE338E5687E90A5FBBDE4` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/route-frame-ms.json | 8586 | `B0DF783F494EFBA00A898CA014470B81FAF7667497D060A3BC3957C968AA838D` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/cold-owned-alpha-proof.json | 505 | `3DF241C020C5F9732014687B6F356CD21EF0C863EB2701FFB366FAFD977E5AA6` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/archive-source-binding-proof.json | 469 | `8FE3A4332E40730FDE97BC5694EB6E10C793D43D85584ED81365AB122D387677` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/returned-settled.png | 2163016 | `A83F6BA4D76B7E0079880A6A5FA2E1E8C046FB84A9D7DBD8694E13B43878048F` |
| build/dayz-cold-startup-source-c822b64a/scripts/Test-DayZStreamingLatency.ps1 | 43230 | `7F7152BDB4804851B72DAC8FC54218291DEDD1A6D6BBFE47863CDDD921B8B8FA` |
