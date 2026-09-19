# Complete incoming-main inventory, 2026-09-19

Follow-up: the 21 TAKE-P1/P2 packages are now source-integrated; see [COMBAT-EXPANSION-20260919](COMBAT-EXPANSION-20260919.md) for adaptations, verification and delivery. The table below records the original audit, not the updated deployment status.

Compared fork `f380053b` (`parity/foliage-lighting-001`) with fetched upstream
`ce7c153746d659359260934c5470e46650e81f83`. Common ancestor:
`a6e3f2fcd7acd3a513cb595b7398ba4597f35427`.

## Coverage and limits

There are **84 incoming non-merge commits** in `HEAD..upstream/main`.
Git patch equivalence accounts for two: `d8870c11` (Ref move tests) and
`a9cdfc29` (map wheel bindings). All remaining **82** have a row below.
Thus no incoming main commit was omitted merely because its author date is old.
Changes inherited through the common ancestor are already in branch history;
this is not a re-audit of every inherited line for later regressions.

Method: enumerate the full divergent range, inspect changed-file scope,
reverse-apply checks without writes, compare substantive additions with current
source, then inspect current implementations of candidate fixes. A line match
or reverse-patch match is evidence of code presence, not runtime correctness.
PRESENT means the production correction is present; tests can differ.
ADAPTED means a divergent local implementation warrants preservation, not that
all edge cases have been independently proved. Platform/test rows retain their
specific follow-up scope instead of claiming complete runtime equivalence.

This is an audit only: no game code changed, no game launch, no replacement of
the `f380053b` EXE/DLL the owner is about to test.

## Recommended order

1. **Mission loading and mod filenames:** `230dd173`, `df5d30f5`, `33fd6ce1`.
   Current code still escalates warning severity and uses ANSI-sensitive paths
   in directory enumeration/archive extraction. Test a warning-only mission and
   round-trip names with umlauts, Czech and Cyrillic characters. Preserve the
   fork's native-world resolver when later adding island/addon validation.
2. **KI logging:** `d3f93039` plus `de34f16e`. The current code still treats
   foreign messages on a shared center channel as failed AI checks and reports
   abstract entities through a repeating check. These can add log noise/work in
   a large mission. No measured FPS improvement is claimed. Upstream's helper
   actually warns once even on the center channel, despite its title/comment;
   define the intended channel semantics before adapting, and keep genuine
   group-channel mistakes visible. Do not globally disable AI_ERROR.
3. **Audio correctness:** `bab72482`. Preserve fractional fade accumulation,
   initial streaming offsets, and stereo skip positions. This is OpenAL, not
   GL33, and is relevant to the fork. Port its focused audio regressions.
4. **Small test/UI repairs:** portable PAA temp paths (`0e24d744`), gear HTML
   table cell widths (`d9192a25`), MP version precision (`ce7c1537`).
5. **Larger coherent packages:** resumable downloads + feedback; MP mission
   folders/duplicate discovery; mission title localization; shared bindings and
   bindable chat controls. Keep each package separate from the helicopter test.
6. **WGPU-specific follow-ups:** proxy conform state, remount detail textures,
   LOD-dependent deformation. GL33 terrain instancing is not their implementation.
   Opaque sorting/discardable-buffer work remains behind the previous measured
   evidence gate; no transferable upstream 10x/28% FPS claim.

## Earlier PR candidates are a separate queue

The September 13 cross-audit also cited PR-head hashes, not only merged main.
Locally available `a5268534`, `9eff4a62`, `2445dc39`, `0c82386c`,
`759a2803`, `0a8b4321`, and `519a46f8` are **not ancestors of current main**.
Some other old hashes are not locally available. Do not present these as missed
merged commits or use their historical PR status as current evidence.

Still useful targeted candidates from that queue: Tank `_doGearSound`
initialization (still absent), invalid path coordinates, static-weapon crew
retention, seated leg transforms and remount lifetimes. Head initialization and
street-lamp fixes were already adapted; preserve their existing behavior.
Their prior runtime gates remain open; this inventory does not approve a bulk
cherry-pick or certify live PR mergeability.

## Every unmatched incoming commit

TAKE-P1/P2 are recommendations, **not newly implemented changes**.

| Commit | Upstream change | Disposition | Evidence / next action |
| --- | --- | --- | --- |
| [ce7c1537](https://github.com/ofpisnotdead-com/CWR-CE/commit/ce7c153746d659359260934c5470e46650e81f83) | [Display Text] Optimize MP room version display text (#236) | TAKE-P2 | MP room version precision improvement; low-priority UI fix, not performance. |
| [c1f76798](https://github.com/ofpisnotdead-com/CWR-CE/commit/c1f76798742789d366c32f4a8a51c035b7963f96) | Move terrain toggle to Render tab | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [b534fd79](https://github.com/ofpisnotdead-com/CWR-CE/commit/b534fd79b9e3dacec9e1c451f47a2dd2cd6fdcac) | Update terrain shader count | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [b20613b0](https://github.com/ofpisnotdead-com/CWR-CE/commit/b20613b0d967fb0756bbb77a5c68deb40f3f59e5) | test: adjust GLSL validation rules | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [82147189](https://github.com/ofpisnotdead-com/CWR-CE/commit/82147189e8f7ca053414f99aa7fb0a04a15f586c) | perf: implement instanced terrain rendering | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [8c61786d](https://github.com/ofpisnotdead-com/CWR-CE/commit/8c61786ddba773dc13fc9ddb1f5e0aaa5ac93a5c) | Set the land clip mode before drawing proxies | WGPU-REVIEW | GL state setter absent by design. WGPU direct/proxy conform-state regression is still worth retaining. |
| [77f1f8ce](https://github.com/ofpisnotdead-com/CWR-CE/commit/77f1f8ce817e1767ac3af80abbd8ded31a2a6068) | Fixed addons warning logging. (#308) | PRESENT | Patch/content already present; no duplicate port. |
| [0e24d744](https://github.com/ofpisnotdead-com/CWR-CE/commit/0e24d7445a21261e94fb42897e6056b918c631a9) | Fix Windows PAA encoder test temp paths (#264) | TAKE-P1 | PAA encoder tests still use /tmp/paa_test_; switch to existing portable test temp helpers. |
| [3541affa](https://github.com/ofpisnotdead-com/CWR-CE/commit/3541affa8027e92e072a5ef899b588a31823100b) | Extract object batching helpers | PARK-MEASURED | Prior audit measured little/no benefit on active WGPU paths; retain evidence gate. |
| [6f0d7fec](https://github.com/ofpisnotdead-com/CWR-CE/commit/6f0d7fecf7e85b9c1858c4fb445139ea9c8c5782) | Release GL33 instancing buffers | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [cc4bd699](https://github.com/ofpisnotdead-com/CWR-CE/commit/cc4bd699d14d9214a75a694106eea823f0122d86) | perf: optimize shape sorting / grouping | PARK-MEASURED | Prior audit measured little/no benefit on active WGPU paths; retain evidence gate. |
| [c551e783](https://github.com/ofpisnotdead-com/CWR-CE/commit/c551e7838816ad63302e261c22e96dd6d1613068) | perf: enable instancing for locally lit objects | WGPU-REVIEW | Do not import GL UBO/light-array API. WGPU already has local-light infrastructure; audit any remaining batch exclusion independently. |
| [ed6119f9](https://github.com/ofpisnotdead-com/CWR-CE/commit/ed6119f9f65244231ed0cc393e5e238bc98d3c5f) | Reload the detail textures when game data unloads | WGPU-REVIEW | GL texture-cache change not directly portable. WGPU detail-resource remount lifetime and matching regression remain open. |
| [a61130ca](https://github.com/ofpisnotdead-com/CWR-CE/commit/a61130ca3d78e5e96b284bc6f1675eb1eaa847a9) | Reset the scene preloader when game data unloads | ADAPTED | Shutdown calls ScenePreloader::Shutdown; reinitialization support present. Only documentation differs in header. |
| [6a63f96d](https://github.com/ofpisnotdead-com/CWR-CE/commit/6a63f96dd91e1526d25fcc91e5ea97d4581c343a) | Add perl-open to Fedora dependencies | PLATFORM | Linux/arm64/build-documentation follow-up; not needed for current Windows WGPU test. |
| [7d69cbe3](https://github.com/ofpisnotdead-com/CWR-CE/commit/7d69cbe3eb15899d631e11bf5b796042b96a13ee) | Fix flaky and stale Trident tests | TEST-REVIEW | Trident log-tap/mark assertions and test updates missing; useful for reliable bounded checks. |
| [4c6aff84](https://github.com/ofpisnotdead-com/CWR-CE/commit/4c6aff849d1bee101ea8d1c813b1866ba138e58e) | Scale every cursor off the layout canvas | ADAPTED | CursorLayout files match upstream. Remaining screen-text difference is literal 600 vs LayoutCanvas::kHeight=600; equivalent here. |
| [078bb2f7](https://github.com/ofpisnotdead-com/CWR-CE/commit/078bb2f7714f57139262dca0c606a5410df79142) | Fix stretched cursor on widescreen | ADAPTED | Superseded by the shared CursorLayout::ComputeRect path already present. |
| [bc3014a9](https://github.com/ofpisnotdead-com/CWR-CE/commit/bc3014a96eee37d39b0c083322b9f4caffacbb59) | Preserve uniform buffer binding state | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [a918a023](https://github.com/ofpisnotdead-com/CWR-CE/commit/a918a0232a7642377bafb31553a2a14951166967) | Release GL33 heightmap texture | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [9a298d09](https://github.com/ofpisnotdead-com/CWR-CE/commit/9a298d09fd225c7019ce6195dc012950ece4833a) | Preserve animated bounds state | ADAPTED | Fork explicitly ports the warm bounds lookup and avoids nested Animate/Deanimate in AnimatedBSphere. |
| [7ca93a57](https://github.com/ofpisnotdead-com/CWR-CE/commit/7ca93a57fe6fec150140d5f3f26b630721387fde) | Exclude destructive deformation from instancing | ADAPTED | DeformsSharedShape exclusions present on first and following batch members. |
| [9a02f8ec](https://github.com/ofpisnotdead-com/CWR-CE/commit/9a02f8eca8887bbb589797110b613fb522b9b733) | Handle ForestPlain GPU land clipping | WGPU-REVIEW | ForestPlain deformation concern is relevant; current fork has its own conform path. No GL heightmap authority added. |
| [b04ae1ae](https://github.com/ofpisnotdead-com/CWR-CE/commit/b04ae1ae54983672f6473215f9200b9521d86126) | Test GL33 land clip decisions | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [315733ab](https://github.com/ofpisnotdead-com/CWR-CE/commit/315733ab32b2e06d9be31274fcf3aa396abcba00) | Invalidate heightmap texture when parameters change | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [f845a731](https://github.com/ofpisnotdead-com/CWR-CE/commit/f845a7318cfb29f349b3446f00362292bcc620a1) | perf: fix instancing for objects where main LOD uses land clip but higher levels don't | WGPU-REVIEW | LOD-dependent land clipping must be checked against the WGPU path, not copied from GL instancing eligibility. |
| [ef6534b1](https://github.com/ofpisnotdead-com/CWR-CE/commit/ef6534b1ea9c0ffa62a0ce94aa997c4f4e3cb699) | perf: fix instancing and enable instancing for land clipped (mostly vegetation) objects | WGPU-REVIEW | GL instancing/type-removal bundle; preserve WGPU buffer lifetime and deformation semantics. |
| [02c0d741](https://github.com/ofpisnotdead-com/CWR-CE/commit/02c0d741c3a436252c6e7b5282229309c8c8fb09) | debug: add POSEIDON_LOD_FIX env variable for disabling adaptive LOD system | PARK-MEASURED | Prior audit measured little/no benefit on active WGPU paths; retain evidence gate. |
| [d3f93039](https://github.com/ofpisnotdead-com/CWR-CE/commit/d3f930394b88bdf393ffeedc3097eeb0b6bee851) | Stop logging foreign radio commands on the center channel | TAKE-P1 | CommandSent still checks foreign-group messages as AI_ERROR twice; common center radio channel makes this suspicious. Correct semantics before rate-limiting logs. |
| [de34f16e](https://github.com/ofpisnotdead-com/CWR-CE/commit/de34f16eaa011057d1835ab2571aa76777328f68) | Log an abstract entity type once per type | TAKE-P1 | VehicleAI constructor still uses AI_ERROR(!type->IsAbstract()); upstream per-type warning infrastructure is absent. |
| [613f5fde](https://github.com/ofpisnotdead-com/CWR-CE/commit/613f5fde45bce6ebfda88382063f46aee3d72271) | Make download feedback generic | TAKE-P2 | Take with download-resume work; generic feedback includes install failure/ETA state. |
| [f2b39cd9](https://github.com/ofpisnotdead-com/CWR-CE/commit/f2b39cd99f94ff84fdb36678285a607ebcb7907a) | Resume interrupted downloads | TAKE-P2 | DownloadWorker still has one-shot jobs; upstream resumable transfer/retry support absent. Port with cancellation/partial-file tests. |
| [c0226c5d](https://github.com/ofpisnotdead-com/CWR-CE/commit/c0226c5d5fb1320cc03ddefb82fe872e7a0c752a) | Log missing preprocessor includes | PRESENT | Patch/content already present; no duplicate port. |
| [409d14d9](https://github.com/ofpisnotdead-com/CWR-CE/commit/409d14d9c4f696e37dc4c89714e97aab64844e0d) | Fall back from malformed mod resources | PRESENT | Patch/content already present; no duplicate port. |
| [d9192a25](https://github.com/ofpisnotdead-com/CWR-CE/commit/d9192a252d15002307817fea34c0cc5b66f6f67b) | Keep the gear page arrows on the unit name line | TAKE-P2 | Missing/partial upstream feature; integrate as a coherent subsystem change with current fork behavior. |
| [bab72482](https://github.com/ofpisnotdead-com/CWR-CE/commit/bab72482280bed7ab6e0648f36da36c680b6eac6) | Fix music fades and offsets | TAKE-P1 | WaveOAL still resets initial streaming offset to zero, skips without channel multiplier and updates _gainSet even when no AL_GAIN update occurs. |
| [90f38746](https://github.com/ofpisnotdead-com/CWR-CE/commit/90f387466f8c913bf57d512e6d933cd96efcbbb6) | Fix Mods menu feedback | TAKE-P2 | Missing/partial upstream feature; integrate as a coherent subsystem change with current fork behavior. |
| [33fd6ce1](https://github.com/ofpisnotdead-com/CWR-CE/commit/33fd6ce19b1965d54475c30e7af25f6e7bf65f0a) | Write installed mod files under their UTF-8 names | TAKE-P1 | ModArchive::Unpack still constructs narrow fs::path and calls fopen on outPath.string(). |
| [df5d30f5](https://github.com/ofpisnotdead-com/CWR-CE/commit/df5d30f531c09dc08aa5453d59a896c407f775e8) | Read Windows directory names as UTF-8 | TAKE-P1 | Windows Utf8Find wrapper absent. Needed for non-ANSI mission/mod directory names; review fixed-buffer truncation/collisions during port. |
| [b54e468e](https://github.com/ofpisnotdead-com/CWR-CE/commit/b54e468e81e5f72c053c32f444d5624919f14001) | Browse multiplayer missions by category folder | TAKE-P2 | Missing MP folder navigation; take with duplicate enumeration and Unicode/name handling. |
| [04189666](https://github.com/ofpisnotdead-com/CWR-CE/commit/04189666c54a5cbc8beaefc03193432904c242b4) | Refuse single missions whose addons are missing | TAKE-P2 | Missing mission addon validation/helpers; port with dependency reporting, not blanket refusal of currently supported cross-generation content. |
| [230dd173](https://github.com/ofpisnotdead-com/CWR-CE/commit/230dd173d1b76a87edc056a0d36a6dba12e53ba9) | Start missions that raise a warning while loading | TAKE-P1 | WarningMessageLevel still stores level+1; warning can latch EMError and block mission loading. |
| [f6572308](https://github.com/ofpisnotdead-com/CWR-CE/commit/f6572308c72f31a7923d4e506041d7cf35bfb3db) | Decode mission names written in a legacy codepage | TAKE-P2 | Missing/partial upstream feature; integrate as a coherent subsystem change with current fork behavior. |
| [6c35ff79](https://github.com/ofpisnotdead-com/CWR-CE/commit/6c35ff7970cefe8a1dba8fe78a634c8003c44d5f) | Refuse single missions whose island is not installed | TAKE-P2 | Missing standard mission-island availability check; adapt to native/generated world resolution rather than requiring only legacy WRP lookup. |
| [e88327cb](https://github.com/ofpisnotdead-com/CWR-CE/commit/e88327cb1ee1dc61667e8e1b6dfbeb1569ee0e7e) | Read a packed mission's own stringtable for its title | TAKE-P2 | Missing/partial upstream feature; integrate as a coherent subsystem change with current fork behavior. |
| [fe737ea0](https://github.com/ofpisnotdead-com/CWR-CE/commit/fe737ea0eb0515398ca26a0eef81c8a66ee2f905) | Drop the file extension from packed mission rows | TAKE-P2 | Missing/partial upstream feature; integrate as a coherent subsystem change with current fork behavior. |
| [4bcddd8f](https://github.com/ofpisnotdead-com/CWR-CE/commit/4bcddd8f22a01f249bd28dd742a4d36a2d458877) | List a mod's multiplayer missions once | TAKE-P2 | Missing/partial upstream feature; integrate as a coherent subsystem change with current fork behavior. |
| [19db42bb](https://github.com/ofpisnotdead-com/CWR-CE/commit/19db42bba842eaa96632f9e93a4cf2713899fa63) | Resolve mod file paths that escape their own prefix | PRESENT | Patch/content already present; no duplicate port. |
| [881f9669](https://github.com/ofpisnotdead-com/CWR-CE/commit/881f96691652ac6ab038d793393f0f4e5b1bcef9) | Read map file lines without narrowing to char | PRESENT | Patch/content already present; no duplicate port. |
| [2054dbc8](https://github.com/ofpisnotdead-com/CWR-CE/commit/2054dbc870978c0eef48b58cbeece4817862df59) | Widen the soldier motion edge cost field | PRESENT | Patch/content already present; no duplicate port. |
| [718ab3c9](https://github.com/ofpisnotdead-com/CWR-CE/commit/718ab3c979e66d769bc30a1853e5fa362cfbf9c4) | Turn on the stack protector for Linux builds | PLATFORM | Linux/arm64/build-documentation follow-up; not needed for current Windows WGPU test. |
| [73302d1d](https://github.com/ofpisnotdead-com/CWR-CE/commit/73302d1df4d06bbb606f1fc1b0b634013f505c96) | Build optimized Linux targets with `_FORTIFY_SOURCE=2` | PLATFORM | Linux/arm64/build-documentation follow-up; not needed for current Windows WGPU test. |
| [f9057657](https://github.com/ofpisnotdead-com/CWR-CE/commit/f90576572433f9aed366c7a3b4782076ca4a5931) | Test the copy destination mode and long config paths | TEST-REVIEW | Production fileCopy/realpath fixes present; their POSIX regression additions are absent. |
| [9d8b7c54](https://github.com/ofpisnotdead-com/CWR-CE/commit/9d8b7c542afac1423c839ab08495aff0b118feac) | Size the realpath() buffer in ParamFile::Parse() to PATH_MAX | PRESENT | Patch/content already present; no duplicate port. |
| [f351ad38](https://github.com/ofpisnotdead-com/CWR-CE/commit/f351ad382a601ba679fd2b544ff0ac73f3cf63d2) | Pass a file mode to open() when creating a copy destination | PRESENT | Patch/content already present; no duplicate port. |
| [08e67e93](https://github.com/ofpisnotdead-com/CWR-CE/commit/08e67e93e5da57aa663b15dbbe9384f0787070fb) | Initialize the vertex table min/max dirty flag | PRESENT | Patch/content already present; no duplicate port. |
| [fa3a65dd](https://github.com/ofpisnotdead-com/CWR-CE/commit/fa3a65dd0753bba22b0cfc5a90e59e70e0abebbb) | Keep the class name alive while comparing it | PRESENT | Patch/content already present; no duplicate port. |
| [ea7602ce](https://github.com/ofpisnotdead-com/CWR-CE/commit/ea7602ced6fc83d81fd184f643d3479264420bc3) | Give the dummy texture a defined size | PRESENT | Patch/content already present; no duplicate port. |
| [589b4123](https://github.com/ofpisnotdead-com/CWR-CE/commit/589b41230ae194f4e97460aeab2f9641fe00adc6) | Report a bad model instead of aborting the viewer | PRESENT | Patch/content already present; no duplicate port. |
| [3147548f](https://github.com/ofpisnotdead-com/CWR-CE/commit/3147548fa5dd48822c4a1fcf803a87ebea455993) | Remove dead `.gitattributes` rules | PLATFORM | Linux/arm64/build-documentation follow-up; not needed for current Windows WGPU test. |
| [16d39136](https://github.com/ofpisnotdead-com/CWR-CE/commit/16d39136eabd5e53cf3d8476577dae9886323f56) | Make open chat controls bindable | TAKE-P2 | Missing bindable in-chat channel/history controls; separate from already-present map wheel binding. |
| [9d362dd3](https://github.com/ofpisnotdead-com/CWR-CE/commit/9d362dd3588cf7cf737cdb8bedfa8608c5b725dd) | Allow shared control bindings | TAKE-P2 | Missing explicit shared-binding conflict workflow; preserve current context/device behavior. |
| [29b6c5be](https://github.com/ofpisnotdead-com/CWR-CE/commit/29b6c5bea1f88d988e6eec643879819083f9a598) | Correct the dist directory named in the Linux build guide | PLATFORM | Linux/arm64/build-documentation follow-up; not needed for current Windows WGPU test. |
| [6db0d1aa](https://github.com/ofpisnotdead-com/CWR-CE/commit/6db0d1aa16e7e1c8dd72f81df2777148102ffc53) | Make GL33 state caches testable | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [4bc72d42](https://github.com/ofpisnotdead-com/CWR-CE/commit/4bc72d42829fa5bb56387ce01d545d8c895797a0) | Test GL33 bind cache behavior | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [780f50c1](https://github.com/ofpisnotdead-com/CWR-CE/commit/780f50c1375bfc119cd8d1296da33150f747e640) | Reset the GL33 upload cache | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [e19872ca](https://github.com/ofpisnotdead-com/CWR-CE/commit/e19872ca7a4699a3c2bc710d87aafdaa4233dc29) | perf: skip redundant polygon offset state changes | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [8a338067](https://github.com/ofpisnotdead-com/CWR-CE/commit/8a338067693e8f728ff1ac429b45aadaf690b27b) | perf: eliminate redundant UBO uploads | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [4b3a2070](https://github.com/ofpisnotdead-com/CWR-CE/commit/4b3a207085f3c451587209dc5b6c1e2fe786512a) | perf: use incremental state updates in ApplyPipeline | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [f5d08d3a](https://github.com/ofpisnotdead-com/CWR-CE/commit/f5d08d3ab52ac32945ea20ca3e18aed69166128c) | perf: cache UBO bindings | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [4ffc0511](https://github.com/ofpisnotdead-com/CWR-CE/commit/4ffc0511972a704e94918d26939f3946bd032e2b) | perf: skip unnecessary texture unit OpenGL calls | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [03413fe9](https://github.com/ofpisnotdead-com/CWR-CE/commit/03413fe91ca3c2fc0087c1f136fec4d7a39b4092) | chore: remove discardable vertex buffer types | PARK-MEASURED | Prior audit measured little/no benefit on active WGPU paths; retain evidence gate. |
| [406aaff0](https://github.com/ofpisnotdead-com/CWR-CE/commit/406aaff09eb655d7dc1263dacf72ed31691d1636) | perf: do not treat VBSmallDiscardable buffers as dynamic | SKIP-GL33 | GL33 implementation/tests; no WGPU port recommended. |
| [c8229780](https://github.com/ofpisnotdead-com/CWR-CE/commit/c8229780a404e7ba49d86e33e8fc38165834f6ff) | Test land-clipped bounds caching | TEST-REVIEW | Bounds feature exists; compare current tests before adding upstream coverage. |
| [816e615d](https://github.com/ofpisnotdead-com/CWR-CE/commit/816e615ddbc759dc17afe8080dccdec5af306293) | Preserve radix sort stability | PRESENT | Patch/content already present; no duplicate port. |
| [90f90ec4](https://github.com/ofpisnotdead-com/CWR-CE/commit/90f90ec4dde16e3d85e444ac74538720c826c4fb) | chore: move land clip to a separate object method | WGPU-REVIEW | Land-clip refactor is not a standalone benefit for the divergent WGPU conform implementation. |
| [775a2625](https://github.com/ofpisnotdead-com/CWR-CE/commit/775a26259b59a2ec6653c457e705f4fd522fdaf0) | perf: cache Object::AnimatedMinMax bounds for land clipped objects (like trees) | ADAPTED | Animated-bounds cache present with fork predicate IsAnimated; exact upstream applicability differs. |
| [8f3de01c](https://github.com/ofpisnotdead-com/CWR-CE/commit/8f3de01c8a7b7a24d7bacb2f74491abc40066125) | perf: optimize render sorting with cached sort keys and radix sort | ADAPTED | Cached keys/radix sort present; preserve subsequent stable-descending fix. |
| [694f7497](https://github.com/ofpisnotdead-com/CWR-CE/commit/694f749724a9264ad83e4eb34da5e1ebd7d86caa) | Build and sharpen the arm64 char regression tests | TEST-REVIEW | Network signed-char definitions present; audit regression registration/LZW coverage separately. |
| [6bc729ab](https://github.com/ofpisnotdead-com/CWR-CE/commit/6bc729ab17799cb1b20b8d55ce9e82a91a80e7bf) | Fix san and tsan builds | PLATFORM | Linux/arm64/build-documentation follow-up; not needed for current Windows WGPU test. |
| [d7a89671](https://github.com/ofpisnotdead-com/CWR-CE/commit/d7a89671bccbfd252fb0904a5f3b48ab9a716362) | Linux arm64 | PLATFORM | Production signed-char network fields present; LZW checksum writes also signed. Raw byte read differs but does not convert byte values; arm64 build/CI remains separate. |
| [e4a18a10](https://github.com/ofpisnotdead-com/CWR-CE/commit/e4a18a10b048c72a6a0b14f199cc8003a8bca69d) | perf: implement move constructors for Ref<T> | PRESENT | Patch/content already present; no duplicate port. |
