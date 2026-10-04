# Upstream runtime integration receipts — 2026-10-04

This record maps the inherited 102-commit audit and all 54 freshly fetched open-PR references to the bounded runtime port branch. It is a source review and integration guide. The root session owns the combined build, installation, runtime acceptance and authoritative modernisation ledger.

## Pinned scope and evidence

- Backup checkpoint: `8b93dec7`; port base: `de493711`.
- Port source: `d28f4a62a209586de382d267e8b47ef2b84dc480`, branch `codex/upstream-runtime-20261004`.
- Upstream main: `b36cddc7ef745da86d308038108986b1641b574a`; all 54 `ofpisnotdead/pr/<number>` refs were available locally.
- [Machine-readable receipts](UPSTREAM-RUNTIME-RECEIPTS-20261004.json) contain exact upstream heads/bases, binary patch hashes, changed paths, matching whole-file blob identities and the original audit verdicts/notes.
- Original owner audit: `CWR-CE_Audit_cave-mesh_b5dc607_2026-10-04.json`, SHA-256 `a36fbbb1202f2bc8a2ce61666135e2b22b1d022ff2aa3236810b0b703756726d`.

Fresh nonmutating checks found 16 main patches and five PR patches whose complete reverse patch applies to the pinned port tree. No inspected upstream head is an exact ancestor of that tree. These are useful literal checks, not a semantic completeness census: fork refactors can preserve a fix while changing its diff. A failed literal check does not establish a missing feature. No full private-history patch-ID census or runtime-equivalence claim is made here. PRs #3 and #6 have no common ancestor with current upstream main; their historical conditional verdicts remain unproved.

## Implemented runtime slices

| Upstream | Port commits | Behavior and preserved fork contract |
|---|---|---|
| #239 | `1e5811ae` | Retain the active ModCollection while iterating the join list. |
| #173 | `2ad8dbfa` | Initialise Tank gear-sound bitfield; existing Head random-lip initialisation retained. |
| #237 | `31e4b1bd` | Reject nonfinite source and destination path coordinates before unlock/grid conversion; finite representable grid range required. |
| #238 | `c27418b7` | Missing slot/null bounds guards across 21 files; existing Reverse and CalculateAimWeapon guards preserved. Invalid infantry slots return before pose mutation. |
| #209 | `70e75bad` | Retain settled driver/gunner/commander assignment on an occupied static weapon. |
| #92, non-Metal slice | `8e21f670` | Decode marked non-PAA-suffix PAC streams through the PAA reader; separate reader choice preserves suffix-based source proof and retail PAC admission. Existing full 8-bit AI88 conversion retained. |
| #168 | `d7baa7eb`, `24cd858e` | Real abortable harness join, client shutdown, atomic UDP flags and narrow error/string/test fixes; local DIAG services preserved. Unix send abort avoids SIGPIPE. |
| #300 | `e1fca310`, `34849e3f` | Use ATL-style position parsing for drop particles, with the upstream mission/Trident fixture companions. |
| #73 | `53ec2ed6` | Correct full-horizontal turret elevation calculation. |
| #72 | `bc9e7468`, `43428b47` | Honour configured isBinocular/isNVG fields, including the required declaration; stock-name fallbacks preserved. |
| #290 | `436fafbf` | Permit OS close while mission input is disabled; preserve enabled-input menu behavior. |
| #79 | `a7c6e66d` | Route the four remaining center-radio checks through the fork's bounded helper; center-message filters preserved. |
| #284 | `dae0fae3` | Append explicit log files across restarts and remove the unused empty server log sink. |
| main `02c0d741` | `3575cf43` | Opt-in POSEIDON_LOD_FIX density freeze shared by normal/shadow selection; default governor unchanged. Valid finite positive values pin density; unset disables; any supplied nonpositive/nonfinite value (including 0) uses the upstream highest-detail convention. |
| main `694f7497`, missing slice | `3575cf43` | Genuine negative-Up signed matrix regression; existing signed LZW and improved rot8b cases preserved. |
| main `7d69cbe3` | `5ba67303` | Trident uses actual simulation time and bounded ordered stdout control-log assertions. Missing/overflowed capture fails. Local PID ownership, unique logs and awaited cleanup retained. |
| main `f9057657` | `d28f4a62` | Unix copy-permission and long config-path regression tests. These are excluded on Windows and are not Windows platform acceptance. |

#290's authoritative upstream head is `60147dae6b480ab14ae5d5684e27866f9e2169fc`. The `436fafbf` commit message expanded an abbreviated upstream hash incorrectly; this receipt corrects its attribution without rewriting shared history. Source PR/author attribution for the other slices is in their commits and the exact PR-head receipts.

#305's leg-transform recalculation is owned by the ragdoll agent and is outside this branch. Root owns the CrashContext (`a13cac07`), build/CI and packaging slices. Root reported integration of main Linux documentation `29b6c5be`/`6a63f96d`, Tools artifact `7bcb5cb6` and the fork's OpenPoseidon.pdb spelling; these are not claimed as ports by this branch.

## Checks and acceptance boundary

`git diff --check` and Rust formatting (edition 2021) passed. Actual new path/LOD header helpers compiled and passed under Clang O0 and O2 without fast-math. The test source SHA-256 is `4e1d9a0244e61ac74ad12a3fc116b12b3cfee9f41a6068dd6501afee8dadec78`; compiler `D:/Program Files/LLVM/bin/clang++.exe`, C++20. These are bounded helper checks, not full AI or GPU benchmark proof.

After integrating through `5ba67303`, root reported the combined Game/Core/Server/Foundation build passing, Game 33 cases/4895 assertions, retail PAC Core 156 default and 184 opt-in assertions, MLOD 51 cases/1432 assertions, Harness 4 cases/24 assertions and 81 Trident tests. The PAC opt-in run used `WGR_GEOMETRY_PAGE_RETAIL_SOURCE=1`. These parent-run native gates do not substitute for installed gameplay acceptance or Unix tests. `d28f4a62` is a later test-only companion. Root subsequently owns the existing stale Cargo.lock repair and installed OpenPoseidon executable finder follow-up; do not infer installed Trident acceptance from the 81 native/tool tests.

Root should preserve paired companion commits for #72 and #300. Existing native fixtures cover the finite path/LOD admission, SDL close truth table, PAC file/memory/VFS provenance and repeated/blocked harness teardown. The upstream drop-height/fired-nearest-object, simulated vehicle turbo and notebook control-log fixtures are suitable bounded installed checks once root's tool finder and installation are ready. Runtime ports without an actual installed receipt remain acceptance-pending even when their source and native checks pass. Custom isNVG/isBinocular data and real static-weapon assignment need relevant configured fixtures; stock smoke alone cannot prove custom parameter behavior.

## Remaining decisions

The confirmed bounded P1/P2 runtime port list is exhausted on this branch. Remaining conditional or excluded entries are not an instruction to merge every PR:

- Shape bucketing `cc4bd699` and helper extraction `3541affa`, PR #318 and numeric/container performance PRs #95/#105/#111/#112 need fork-specific ordering, aliasing/IEEE behavior and actual cost evidence before promotion. No performance benefit is asserted from upstream code alone.
- PR #172 requires a new fork-specific cosmetic RNG boundary. Explosion damage and other authoritative simulation randomness must remain on the simulation stream; no whole-PR replacement was made.
- GL33/Metal skinning, terrain/water instancing, proxy land clipping and local-light rendering proposals are superseded interfaces here. They require a measured WGPU adaptation, rather than adding a parallel upstream backend. PR #92's Metal/all-mip route and macOS OpenAL lookup/EFX were not ported; the native marked-PAC decoder slice was.
- FORTIFY_SOURCE `73302d1d`, stack protector `718ab3c9`, compiler/ccache/Actions and workflow proposals remain platform/CI decisions owned by root. No untested compiler/platform guarantee is implied.
- Optional UI, marker, scripting, loadout/database/save proposals retain their inherited scope. Closed/unmerged PRs are references only; no new persistence semantics were introduced.

The source license and game-data license remain separate as documented in the repository. This batch adds no retail assets. The marked-PAC fixture is the upstream small synthetic decoder test payload, not an imported game texture. No speculative asset/licence blocker is used to defer these source fixes.

## Complete main-commit mapping

Historical verdicts remain visibly separate from fresh literal checks and this branch's new ports. `HISTORICAL_VERDICT_NOT_FRESHLY_PROVED` means that ancestry/reverse-patch checks did not prove the original verdict; it does not reverse the original review or establish a defect. The JSON retains the exact original notes and target paths.

| Upstream commit | Title | Historical verdict | Fresh literal check | This record |
|---|---|---|---|---|
| [e4a18a10](https://github.com/ofpisnotdead-com/CWR-CE/commit/e4a18a10b048c72a6a0b14f199cc8003a8bca69d) | Ref move constructors | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [d8870c11](https://github.com/ofpisnotdead-com/CWR-CE/commit/d8870c1170d97a7eb3f6ea71b8ead5bb28f1a107) | Ref move tests | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [d7a89671](https://github.com/ofpisnotdead-com/CWR-CE/commit/d7a89671bccbfd252fb0904a5f3b48ab9a716362) | Linux ARM64 | TEILWEISE_OPTIONAL | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [6bc729ab](https://github.com/ofpisnotdead-com/CWR-CE/commit/6bc729ab17799cb1b20b8d55ce9e82a91a80e7bf) | Sanitizer builds fix | OPTIONAL | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [694f7497](https://github.com/ofpisnotdead-com/CWR-CE/commit/694f749724a9264ad83e4eb34da5e1ebd7d86caa) | ARM64 signed-char tests | TEST_ERGAENZEN | not literal | `3575cf43` |
| [8f3de01c](https://github.com/ofpisnotdead-com/CWR-CE/commit/8f3de01c8a7b7a24d7bacb2f74491abc40066125) | Cached sort keys and radix sort | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [775a2625](https://github.com/ofpisnotdead-com/CWR-CE/commit/775a26259b59a2ec6653c457e705f4fd522fdaf0) | Land-clipped animated bounds cache | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [90f90ec4](https://github.com/ofpisnotdead-com/CWR-CE/commit/90f90ec4dde16e3d85e444ac74538720c826c4fb) | Land clip method extraction | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [816e615d](https://github.com/ofpisnotdead-com/CWR-CE/commit/816e615ddbc759dc17afe8080dccdec5af306293) | Radix sort stability | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [c8229780](https://github.com/ofpisnotdead-com/CWR-CE/commit/c8229780a404e7ba49d86e33e8fc38165834f6ff) | Land-clipped bounds cache tests | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [406aaff0](https://github.com/ofpisnotdead-com/CWR-CE/commit/406aaff09eb655d7dc1263dacf72ed31691d1636) | Small discardable buffers not dynamic | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [03413fe9](https://github.com/ofpisnotdead-com/CWR-CE/commit/03413fe91ca3c2fc0087c1f136fec4d7a39b4092) | Remove discardable vertex buffer types | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [a9cdfc29](https://github.com/ofpisnotdead-com/CWR-CE/commit/a9cdfc2907dfb87662102f7de36c453fe08d0d16) | Map mouse-wheel bindings | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [4ffc0511](https://github.com/ofpisnotdead-com/CWR-CE/commit/4ffc0511972a704e94918d26939f3946bd032e2b) | GL texture unit call reduction | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [f5d08d3a](https://github.com/ofpisnotdead-com/CWR-CE/commit/f5d08d3ab52ac32945ea20ca3e18aed69166128c) | UBO bind cache | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [4b3a2070](https://github.com/ofpisnotdead-com/CWR-CE/commit/4b3a207085f3c451587209dc5b6c1e2fe786512a) | Incremental GL pipeline state | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [8a338067](https://github.com/ofpisnotdead-com/CWR-CE/commit/8a338067693e8f728ff1ac429b45aadaf690b27b) | Redundant UBO uploads | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [e19872ca](https://github.com/ofpisnotdead-com/CWR-CE/commit/e19872ca7a4699a3c2bc710d87aafdaa4233dc29) | Skip polygon offset state updates | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [780f50c1](https://github.com/ofpisnotdead-com/CWR-CE/commit/780f50c1375bfc119cd8d1296da33150f747e640) | Reset GL33 upload cache | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [4bc72d42](https://github.com/ofpisnotdead-com/CWR-CE/commit/4bc72d42829fa5bb56387ce01d545d8c895797a0) | GL33 bind cache tests | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [6db0d1aa](https://github.com/ofpisnotdead-com/CWR-CE/commit/6db0d1aa16e7e1c8dd72f81df2777148102ffc53) | Testable GL33 state caches | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [29b6c5be](https://github.com/ofpisnotdead-com/CWR-CE/commit/29b6c5bea1f88d988e6eec643879819083f9a598) | Linux build docs output directory | DOKU_ERGAENZEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [9d362dd3](https://github.com/ofpisnotdead-com/CWR-CE/commit/9d362dd3588cf7cf737cdb8bedfa8608c5b725dd) | Shared input bindings | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [16d39136](https://github.com/ofpisnotdead-com/CWR-CE/commit/16d39136eabd5e53cf3d8476577dae9886323f56) | Bindable chat controls | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [3147548f](https://github.com/ofpisnotdead-com/CWR-CE/commit/3147548fa5dd48822c4a1fcf803a87ebea455993) | Remove dead gitattributes | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [589b4123](https://github.com/ofpisnotdead-com/CWR-CE/commit/589b41230ae194f4e97460aeab2f9641fe00adc6) | P3D viewer error handling | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [ea7602ce](https://github.com/ofpisnotdead-com/CWR-CE/commit/ea7602ced6fc83d81fd184f643d3479264420bc3) | Dummy texture initialization | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [fa3a65dd](https://github.com/ofpisnotdead-com/CWR-CE/commit/fa3a65dd0753bba22b0cfc5a90e59e70e0abebbb) | Entity type name lifetime | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [08e67e93](https://github.com/ofpisnotdead-com/CWR-CE/commit/08e67e93e5da57aa663b15dbbe9384f0787070fb) | VertexTable minMaxDirty initialization | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [f351ad38](https://github.com/ofpisnotdead-com/CWR-CE/commit/f351ad382a601ba679fd2b544ff0ac73f3cf63d2) | fileCopy open creation mode | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [9d8b7c54](https://github.com/ofpisnotdead-com/CWR-CE/commit/9d8b7c542afac1423c839ab08495aff0b118feac) | ParamFile realpath buffer overflow | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [f9057657](https://github.com/ofpisnotdead-com/CWR-CE/commit/f90576572433f9aed366c7a3b4782076ca4a5931) | Copy permission and long path tests | TEST_ERGAENZEN | present | `d28f4a62` |
| [73302d1d](https://github.com/ofpisnotdead-com/CWR-CE/commit/73302d1df4d06bbb606f1fc1b0b634013f505c96) | Linux FORTIFY_SOURCE | BUILD_ABSICHERN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [718ab3c9](https://github.com/ofpisnotdead-com/CWR-CE/commit/718ab3c979e66d769bc30a1853e5fa362cfbf9c4) | Linux stack protector | BUILD_ABSICHERN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [2054dbc8](https://github.com/ofpisnotdead-com/CWR-CE/commit/2054dbc870978c0eef48b58cbeece4817862df59) | Soldier motion edge cost overflow | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [881f9669](https://github.com/ofpisnotdead-com/CWR-CE/commit/881f96691652ac6ab038d793393f0f4e5b1bcef9) | MapFile EOF char narrowing | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [19db42bb](https://github.com/ofpisnotdead-com/CWR-CE/commit/19db42bba842eaa96632f9e93a4cf2713899fa63) | Parent-relative mod paths | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [4bcddd8f](https://github.com/ofpisnotdead-com/CWR-CE/commit/4bcddd8f22a01f249bd28dd742a4d36a2d458877) | Dedup mod multiplayer missions | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [fe737ea0](https://github.com/ofpisnotdead-com/CWR-CE/commit/fe737ea0eb0515398ca26a0eef81c8a66ee2f905) | Mission row extension | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [e88327cb](https://github.com/ofpisnotdead-com/CWR-CE/commit/e88327cb1ee1dc61667e8e1b6dfbeb1569ee0e7e) | Mission PBO stringtable | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [6c35ff79](https://github.com/ofpisnotdead-com/CWR-CE/commit/6c35ff7970cefe8a1dba8fe78a634c8003c44d5f) | Missing mission island | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [f6572308](https://github.com/ofpisnotdead-com/CWR-CE/commit/f6572308c72f31a7923d4e506041d7cf35bfb3db) | Legacy codepage mission names | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [230dd173](https://github.com/ofpisnotdead-com/CWR-CE/commit/230dd173d1b76a87edc056a0d36a6dba12e53ba9) | Mission warning level | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [04189666](https://github.com/ofpisnotdead-com/CWR-CE/commit/04189666c54a5cbc8beaefc03193432904c242b4) | Missing mission addons | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [b54e468e](https://github.com/ofpisnotdead-com/CWR-CE/commit/b54e468e81e5f72c053c32f444d5624919f14001) | MP category folder browsing | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [df5d30f5](https://github.com/ofpisnotdead-com/CWR-CE/commit/df5d30f531c09dc08aa5453d59a896c407f775e8) | UTF-8 Windows directory names | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [33fd6ce1](https://github.com/ofpisnotdead-com/CWR-CE/commit/33fd6ce19b1965d54475c30e7af25f6e7bf65f0a) | UTF-8 mod install paths | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [90f38746](https://github.com/ofpisnotdead-com/CWR-CE/commit/90f387466f8c913bf57d512e6d933cd96efcbbb6) | Mods menu feedback | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [bab72482](https://github.com/ofpisnotdead-com/CWR-CE/commit/bab72482280bed7ab6e0648f36da36c680b6eac6) | Music fades and offsets | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [d9192a25](https://github.com/ofpisnotdead-com/CWR-CE/commit/d9192a252d15002307817fea34c0cc5b66f6f67b) | Gear page arrow layout | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [409d14d9](https://github.com/ofpisnotdead-com/CWR-CE/commit/409d14d9c4f696e37dc4c89714e97aab64844e0d) | Malformed mod resources fallback | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [c0226c5d](https://github.com/ofpisnotdead-com/CWR-CE/commit/c0226c5d5fb1320cc03ddefb82fe872e7a0c752a) | Log missing preprocessor includes | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [f2b39cd9](https://github.com/ofpisnotdead-com/CWR-CE/commit/f2b39cd99f94ff84fdb36678285a607ebcb7907a) | Resume downloads | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [613f5fde](https://github.com/ofpisnotdead-com/CWR-CE/commit/613f5fde45bce6ebfda88382063f46aee3d72271) | Generic localized download feedback | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [de34f16e](https://github.com/ofpisnotdead-com/CWR-CE/commit/de34f16eaa011057d1835ab2571aa76777328f68) | Log abstract entity once per type | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [d3f93039](https://github.com/ofpisnotdead-com/CWR-CE/commit/d3f930394b88bdf393ffeedc3097eeb0b6bee851) | Stop logging foreign radio commands on the center channel | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [02c0d741](https://github.com/ofpisnotdead-com/CWR-CE/commit/02c0d741c3a436252c6e7b5282229309c8c8fb09) | Fixed benchmark LOD | IMPLEMENTIEREN | not literal | `3575cf43` |
| [ef6534b1](https://github.com/ofpisnotdead-com/CWR-CE/commit/ef6534b1ea9c0ffa62a0ce94aa997c4f4e3cb699) | GPU land clipping and instancing | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [f845a731](https://github.com/ofpisnotdead-com/CWR-CE/commit/f845a7318cfb29f349b3446f00362292bcc620a1) | Per-LOD instancing land clip | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [315733ab](https://github.com/ofpisnotdead-com/CWR-CE/commit/315733ab32b2e06d9be31274fcf3aa396abcba00) | Heightmap invalidation setTerrainGrid | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [b04ae1ae](https://github.com/ofpisnotdead-com/CWR-CE/commit/b04ae1ae54983672f6473215f9200b9521d86126) | Land clip mode tests | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [9a02f8ec](https://github.com/ofpisnotdead-com/CWR-CE/commit/9a02f8eca8887bbb589797110b613fb522b9b733) | ForestPlain GPU land clip | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [7ca93a57](https://github.com/ofpisnotdead-com/CWR-CE/commit/7ca93a57fe6fec150140d5f3f26b630721387fde) | Destructive deformation instancing guard | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [9a298d09](https://github.com/ofpisnotdead-com/CWR-CE/commit/9a298d09fd225c7019ce6195dc012950ece4833a) | Animated bounds state | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [a918a023](https://github.com/ofpisnotdead-com/CWR-CE/commit/a918a0232a7642377bafb31553a2a14951166967) | Release GL33 heightmap | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [bc3014a9](https://github.com/ofpisnotdead-com/CWR-CE/commit/bc3014a96eee37d39b0c083322b9f4caffacbb59) | Uniform buffer binding cache | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [078bb2f7](https://github.com/ofpisnotdead-com/CWR-CE/commit/078bb2f7714f57139262dca0c606a5410df79142) | Widescreen cursor | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [4c6aff84](https://github.com/ofpisnotdead-com/CWR-CE/commit/4c6aff849d1bee101ea8d1c813b1866ba138e58e) | All cursor layout scaling | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [7d69cbe3](https://github.com/ofpisnotdead-com/CWR-CE/commit/7d69cbe3eb15899d631e11bf5b796042b96a13ee) | Flaky Trident tests | IMPLEMENTIEREN | not literal | `5ba67303` |
| [6a63f96d](https://github.com/ofpisnotdead-com/CWR-CE/commit/6a63f96dd91e1526d25fcc91e5ea97d4581c343a) | Fedora perl-open | DOKU_ERGAENZEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [a61130ca](https://github.com/ofpisnotdead-com/CWR-CE/commit/a61130ca3d78e5e96b284bc6f1675eb1eaa847a9) | Reset scene preloader | VORHANDEN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [ed6119f9](https://github.com/ofpisnotdead-com/CWR-CE/commit/ed6119f9f65244231ed0cc393e5e238bc98d3c5f) | Reload detail textures | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [c551e783](https://github.com/ofpisnotdead-com/CWR-CE/commit/c551e7838816ad63302e261c22e96dd6d1613068) | Locally lit instancing | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [cc4bd699](https://github.com/ofpisnotdead-com/CWR-CE/commit/cc4bd699d14d9214a75a694106eea823f0122d86) | Shape bucketing | BEDINGT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [6f0d7fec](https://github.com/ofpisnotdead-com/CWR-CE/commit/6f0d7fecf7e85b9c1858c4fb445139ea9c8c5782) | Release GL33 instancing buffers | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [3541affa](https://github.com/ofpisnotdead-com/CWR-CE/commit/3541affa8027e92e072a5ef899b588a31823100b) | Extract batching helpers | TEST_BEI_PORT | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [0e24d744](https://github.com/ofpisnotdead-com/CWR-CE/commit/0e24d7445a21261e94fb42897e6056b918c631a9) | Windows PAA encoder test temp paths | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [77f1f8ce](https://github.com/ofpisnotdead-com/CWR-CE/commit/77f1f8ce817e1767ac3af80abbd8ded31a2a6068) | Addon warning logging | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [8c61786d](https://github.com/ofpisnotdead-com/CWR-CE/commit/8c61786ddba773dc13fc9ddb1f5e0aaa5ac93a5c) | Land clip before proxies | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [82147189](https://github.com/ofpisnotdead-com/CWR-CE/commit/82147189e8f7ca053414f99aa7fb0a04a15f586c) | Instanced terrain rendering | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [b20613b0](https://github.com/ofpisnotdead-com/CWR-CE/commit/b20613b0d967fb0756bbb77a5c68deb40f3f59e5) | GLSL validation rules | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [b534fd79](https://github.com/ofpisnotdead-com/CWR-CE/commit/b534fd79b9e3dacec9e1c451f47a2dd2cd6fdcac) | Terrain shader count | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [c1f76798](https://github.com/ofpisnotdead-com/CWR-CE/commit/c1f76798742789d366c32f4a8a51c035b7963f96) | Terrain toggle render tab | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [ce7c1537](https://github.com/ofpisnotdead-com/CWR-CE/commit/ce7c153746d659359260934c5470e46650e81f83) | MP version display | VORHANDEN | present | LITERAL_PATCH_PRESENT |
| [825b3b15](https://github.com/ofpisnotdead-com/CWR-CE/commit/825b3b15d2c84bf3047eb3f3d4e62022a83f8c09) | Shader include preprocessor | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [ad332672](https://github.com/ofpisnotdead-com/CWR-CE/commit/ad332672ee252426d271ad8db359c969831bd4c2) | Instanced water rendering | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [d70fdbd5](https://github.com/ofpisnotdead-com/CWR-CE/commit/d70fdbd56ed46de6bcca90217f4158bf2ec10fbc) | One terrain instance per segment | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [618bdcd4](https://github.com/ofpisnotdead-com/CWR-CE/commit/618bdcd41b987e79718fa943dfe3dcbfd01ec014) | Remove redundant terrain sampler rebinding | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [235e9181](https://github.com/ofpisnotdead-com/CWR-CE/commit/235e9181e0fbe067be5e5433792ace1340d779c1) | Separate shader sources and tests | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [06bd4a1a](https://github.com/ofpisnotdead-com/CWR-CE/commit/06bd4a1a0b486bc35c280df7a792bde755c4ab30) | Water beyond map boundaries | EIGENE_LOESUNG | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [8d878a08](https://github.com/ofpisnotdead-com/CWR-CE/commit/8d878a0898d26fdbd7f6d2e245b7049a84cc8d31) | Harden terrain segment lookup | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [3b5849e3](https://github.com/ofpisnotdead-com/CWR-CE/commit/3b5849e397123ba915a508c119916d7e3bd4a016) | Shader catalog for cache hashing | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [3ffb0338](https://github.com/ofpisnotdead-com/CWR-CE/commit/3ffb0338c0c7c62d1d4c8bfb9a0dd37923dedebf) | Link instanced ground shaders in tests | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [b67bf3bd](https://github.com/ofpisnotdead-com/CWR-CE/commit/b67bf3bd623a52d62456c3ef65f7e0e0163dcd9b) | Enable instanced terrain by default | NICHT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [dc42d84f](https://github.com/ofpisnotdead-com/CWR-CE/commit/dc42d84f73808b0fe09d7b61ef987f6aa0c60cd8) | Key the Windows ccache on the MSVC version | BUILD_ABSICHERN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [a13cac07](https://github.com/ofpisnotdead-com/CWR-CE/commit/a13cac078a6a6be260438266fd869fd184c0a330) | Give dbghelp a full-size copy of the crash context | IMPLEMENTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [7bcb5cb6](https://github.com/ofpisnotdead-com/CWR-CE/commit/7bcb5cb64f116308b0455b26155c1a32e3309011) | Upload also tools | OPTIONAL | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [5883e924](https://github.com/ofpisnotdead-com/CWR-CE/commit/5883e9244117b8fa00ff856e34978a0158f65674) | Update GitHub Actions to Node 24 releases | BUILD_ABSICHERN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [c2fd4c2e](https://github.com/ofpisnotdead-com/CWR-CE/commit/c2fd4c2e91f19054b2f0ce0f171ac0d5c4346727) | Install LLVM without install-llvm-action | BUILD_ABSICHERN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [b453df21](https://github.com/ofpisnotdead-com/CWR-CE/commit/b453df2147b3ed633038b93d89f2a045449d6d23) | Drop vcpkg_copy_pdbs from the openal-soft port | OPTIONAL | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [19bc48a0](https://github.com/ofpisnotdead-com/CWR-CE/commit/19bc48a0f9cf4204775feca7f978216ea8675235) | Add Dependabot for GitHub Actions | OPTIONAL | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |
| [b36cddc7](https://github.com/ofpisnotdead-com/CWR-CE/commit/b36cddc7ef745da86d308038108986b1641b574a) | Run CI through one workflow with a gate job | BEDINGT_PORTIEREN | not literal | HISTORICAL_VERDICT_NOT_FRESHLY_PROVED |

## Complete open-PR mapping

Every row refers to the fresh local PR head pinned in the JSON. “Not literal” retains the historical conditional/excluded verdict; it is not an implementation request or a statement that the fork lacks the behavior.

| PR | Title | Historical verdict | Fresh literal check | This record |
|---|---|---|---|---|
| [#319](https://github.com/ofpisnotdead-com/CWR-CE/pull/319) | Add MacOS build docs. | OPTIONAL | not literal | OPTIONAL |
| [#318](https://github.com/ofpisnotdead-com/CWR-CE/pull/318) | perf: batch draw map objects by type | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#310](https://github.com/ofpisnotdead-com/CWR-CE/pull/310) | Survive script-created street lamps | VORHANDEN | not literal | VORHANDEN |
| [#309](https://github.com/ofpisnotdead-com/CWR-CE/pull/309) | Fix FreeType glyph bounds and text clipping (Issue #97) | TEST_NUTZEN | not literal | TEST_NUTZEN |
| [#305](https://github.com/ofpisnotdead-com/CWR-CE/pull/305) | Fix leg transform on sloped vehicles | PORT_ADAPTIERT | not literal | OTHER_AGENT_PORT_PENDING_ACCEPTANCE |
| [#300](https://github.com/ofpisnotdead-com/CWR-CE/pull/300) | Read drop positions as heights above the surface | PORT_ADAPTIERT | present | `e1fca310`, `34849e3f` |
| [#295](https://github.com/ofpisnotdead-com/CWR-CE/pull/295) | Gameplay enhancement: Reloading redundancy | NICHT_PORTIEREN | not literal | NICHT_PORTIEREN |
| [#294](https://github.com/ofpisnotdead-com/CWR-CE/pull/294) | Fixes missing button focus/highlight in the key capture conflict confirmation page. | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#293](https://github.com/ofpisnotdead-com/CWR-CE/pull/293) | Fixes ESCAPE key not working in key capturing conflict confirmation page | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#290](https://github.com/ofpisnotdead-com/CWR-CE/pull/290) | Let Alt+F4 quit while a mission has user input disabled | PORT_ADAPTIERT | not literal | `436fafbf` |
| [#284](https://github.com/ofpisnotdead-com/CWR-CE/pull/284) | Keep --log-file across restarts, and stop writing an empty one | PORT_ADAPTIERT | present | `dae0fae3` |
| [#277](https://github.com/ofpisnotdead-com/CWR-CE/pull/277) | Fix for Issues #193 and #131: open vehicle sights | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#265](https://github.com/ofpisnotdead-com/CWR-CE/pull/265) | Windows temp-path fixes for World state and PreviewImage tests | TEST_NUTZEN | not literal | TEST_NUTZEN |
| [#258](https://github.com/ofpisnotdead-com/CWR-CE/pull/258) | Add numeric option entry | OPTIONAL | not literal | OPTIONAL |
| [#250](https://github.com/ofpisnotdead-com/CWR-CE/pull/250) | Let players keep night in color | NICHT_PORTIEREN | not literal | NICHT_PORTIEREN |
| [#239](https://github.com/ofpisnotdead-com/CWR-CE/pull/239) | Fix UB in DisplayMultiplayer::BeginModdedJoin | PORT_ADAPTIERT | present | `1e5811ae` |
| [#238](https://github.com/ofpisnotdead-com/CWR-CE/pull/238) | TZK CWA-CE 2.03 crash fixes | TEILWEISE_VORHANDEN | not literal | `c27418b7` |
| [#237](https://github.com/ofpisnotdead-com/CWR-CE/pull/237) | Add NaN check in AIUnit::IsSimplePath | PORT_ADAPTIERT | not literal | `31e4b1bd` |
| [#210](https://github.com/ofpisnotdead-com/CWR-CE/pull/210) | AISubgroupFSM: drop invalid assignment assert in GetIn point helpers | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#209](https://github.com/ofpisnotdead-com/CWR-CE/pull/209) | AIGroupCmd: keep static-weapon crew assigned in AssignVehicles | PORT_ADAPTIERT | present | `70e75bad` |
| [#208](https://github.com/ofpisnotdead-com/CWR-CE/pull/208) | Add unit loadout scripting commands | KONZEPT_NEU_UMSETZEN | not literal | KONZEPT_NEU_UMSETZEN |
| [#178](https://github.com/ofpisnotdead-com/CWR-CE/pull/178) | GL33 GPU skinning for infantry view LODs | NICHT_PORTIEREN | not literal | NICHT_PORTIEREN |
| [#173](https://github.com/ofpisnotdead-com/CWR-CE/pull/173) | Fix uninitialised per-entity heap fields read during simulation | TEILWEISE_VORHANDEN | not literal | `2ad8dbfa` |
| [#172](https://github.com/ofpisnotdead-com/CWR-CE/pull/172) | Separate cosmetic-effect RNG from the simulation RNG stream | KONZEPT_NEU_UMSETZEN | not literal | KONZEPT_NEU_UMSETZEN |
| [#168](https://github.com/ofpisnotdead-com/CWR-CE/pull/168) | Fix issues while running macOS tests with sanitizers | PORT_ADAPTIERT | not literal | `d7baa7eb`, `24cd858e` |
| [#112](https://github.com/ofpisnotdead-com/CWR-CE/pull/112) | perf: SSE Vector3P::SetFastTransform | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#111](https://github.com/ofpisnotdead-com/CWR-CE/pull/111) | perf: gate heavy AI validators out of release builds | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#110](https://github.com/ofpisnotdead-com/CWR-CE/pull/110) | Fix --benchmark on POSIX | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#106](https://github.com/ofpisnotdead-com/CWR-CE/pull/106) | MathOpt: use SSE rsqrtss for InvSqrt on x86 | NICHT_PORTIEREN | not literal | NICHT_PORTIEREN |
| [#105](https://github.com/ofpisnotdead-com/CWR-CE/pull/105) | FltOpts: cvtss2si instead of nearbyint | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#102](https://github.com/ofpisnotdead-com/CWR-CE/pull/102) | Options: Replace controls reset with presets page | OPTIONAL | not literal | OPTIONAL |
| [#95](https://github.com/ofpisnotdead-com/CWR-CE/pull/95) | Modify markersMap with an extra hash-map | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#93](https://github.com/ofpisnotdead-com/CWR-CE/pull/93) | Use _this for UserAction caller | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#92](https://github.com/ofpisnotdead-com/CWR-CE/pull/92) | Add a native Metal renderer for macOS | TEILWEISE_VORHANDEN | not literal | `8e21f670` |
| [#88](https://github.com/ofpisnotdead-com/CWR-CE/pull/88) | QIFStreamB: try mod dirs when loose-file open fails on POSIX | VORHANDEN | not literal | VORHANDEN |
| [#86](https://github.com/ofpisnotdead-com/CWR-CE/pull/86) | UnloadGameData: reset ScenePreloader on re-mount | VORHANDEN | not literal | VORHANDEN |
| [#85](https://github.com/ofpisnotdead-com/CWR-CE/pull/85) | Guard null preloaded CraterShell | TEST_NUTZEN | not literal | TEST_NUTZEN |
| [#84](https://github.com/ofpisnotdead-com/CWR-CE/pull/84) | Rotate markers and Alt-big steps | OPTIONAL | not literal | OPTIONAL |
| [#82](https://github.com/ofpisnotdead-com/CWR-CE/pull/82) | VehicleSupply::SupplyStarted: drop duplicate-entry assert | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#81](https://github.com/ofpisnotdead-com/CWR-CE/pull/81) | Man::KilledBy: null-brain corpses | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#80](https://github.com/ofpisnotdead-com/CWR-CE/pull/80) | Object setVariable/getVariable | KONZEPT_NEU_UMSETZEN | not literal | KONZEPT_NEU_UMSETZEN |
| [#79](https://github.com/ofpisnotdead-com/CWR-CE/pull/79) | Skip foreign-sender check on shared center radio | TEILWEISE_VORHANDEN | not literal | `a7c6e66d` |
| [#78](https://github.com/ofpisnotdead-com/CWR-CE/pull/78) | EnableAI and checkAIFeature commands | KONZEPT_NEU_UMSETZEN | not literal | KONZEPT_NEU_UMSETZEN |
| [#77](https://github.com/ofpisnotdead-com/CWR-CE/pull/77) | Expose LOAD MAGAZINE action to scripts | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#76](https://github.com/ofpisnotdead-com/CWR-CE/pull/76) | Add Ctrl state to onMapSingleClick | OPTIONAL | not literal | OPTIONAL |
| [#75](https://github.com/ofpisnotdead-com/CWR-CE/pull/75) | pressingKey scripting command | OPTIONAL | not literal | OPTIONAL |
| [#73](https://github.com/ofpisnotdead-com/CWR-CE/pull/73) | Fix getTurretDirAndElev angle | PORT_ADAPTIERT | present | `53ec2ed6` |
| [#72](https://github.com/ofpisnotdead-com/CWR-CE/pull/72) | Support isBinocular/isNVG weapon parameters | PORT_ADAPTIERT | not literal | `bc9e7468`, `43428b47` |
| [#64](https://github.com/ofpisnotdead-com/CWR-CE/pull/64) | Make map edge scrolling frame-rate independent | VORHANDEN | not literal | VORHANDEN |
| [#51](https://github.com/ofpisnotdead-com/CWR-CE/pull/51) | FreeBSD support | OPTIONAL | not literal | OPTIONAL |
| [#49](https://github.com/ofpisnotdead-com/CWR-CE/pull/49) | Add selectPlayer scripting command | BEDINGT_PRUEFEN | not literal | BEDINGT_PRUEFEN |
| [#10](https://github.com/ofpisnotdead-com/CWR-CE/pull/10) | Refactor network messages to macros | NICHT_PORTIEREN | not literal | NICHT_PORTIEREN |
| [#6](https://github.com/ofpisnotdead-com/CWR-CE/pull/6) | Status Quo multiplayer fixes | BEDINGT_PRUEFEN | no common ancestor | BEDINGT_PRUEFEN |
| [#3](https://github.com/ofpisnotdead-com/CWR-CE/pull/3) | Status Quo simple fixes | BEDINGT_PRUEFEN | no common ancestor | BEDINGT_PRUEFEN |

## Closed/unmerged references

Metadata-only references preserved from the inherited audit. Their diffs were not re-audited in this refresh, and none is marked implemented by this branch.

| PR | Title | Scope |
|---|---|---|
| [#206](https://github.com/ofpisnotdead-com/CWR-CE/pull/206) | Scripting/JSON/database/events/loadout bundle | Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert |
| [#190](https://github.com/ofpisnotdead-com/CWR-CE/pull/190) | Addon conflict/merge-order diagnostics | Geschlossen/unmerged: PR-Diff gelesen, kein vollständiger aktueller Zielcode-Abgleich |
| [#189](https://github.com/ofpisnotdead-com/CWR-CE/pull/189) | Config/bank/type/slot diagnostic dumps | Geschlossen/unmerged: PR-Diff gelesen, kein vollständiger aktueller Zielcode-Abgleich |
| [#165](https://github.com/ofpisnotdead-com/CWR-CE/pull/165) | Application name/path correction | Geschlossen/unmerged: PR-Diff gelesen, kein vollständiger aktueller Zielcode-Abgleich |
| [#163](https://github.com/ofpisnotdead-com/CWR-CE/pull/163) | UTF-8 faces / related changes | Geschlossen/unmerged: PR-Diff gelesen, kein vollständiger aktueller Zielcode-Abgleich |
| [#126](https://github.com/ofpisnotdead-com/CWR-CE/pull/126) | Older combined performance work | Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert |
| [#94](https://github.com/ofpisnotdead-com/CWR-CE/pull/94) | ARM64 support predecessor | Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert |
| [#90](https://github.com/ofpisnotdead-com/CWR-CE/pull/90) | Overlay/control UI adjustment | Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert |
| [#89](https://github.com/ofpisnotdead-com/CWR-CE/pull/89) | External GL shader organization | Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert |
| [#87](https://github.com/ofpisnotdead-com/CWR-CE/pull/87) | Widescreen UI predecessor | Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert |
| [#20](https://github.com/ofpisnotdead-com/CWR-CE/pull/20) | SDL/vcpkg dependency alternative | Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert |
| [#18](https://github.com/ofpisnotdead-com/CWR-CE/pull/18) | Debian/ibus build prerequisite | Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert |
