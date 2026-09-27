# Tidewater series integration into the streaming branch

This checkpoint ports the complete nine-commit series from the verified private safety remote `refs/heads/dec/tidewater-w3-shore`, tip `a2b9c13539301ddc2bdb7127628c098ff00595a5`, onto streaming base `095cee223225e83209b15470afd454324427464b`. The common ancestor is `3d720430c049cbd82cf3757dbf1fca1534702c4f`. Origin has no matching branch. All nine source commits were absent by patch-equivalence comparison and cherry-picked with original authors and `-x` provenance; there were no textual conflicts. This isolated source checkpoint has not been built or smoke-tested.

| Source commit | Port commit | Source scope |
| --- | --- | --- |
| 9b49f270 | a4a3cb51 | W1 backend selection seam |
| b3f2c3f0 | 7891e9eb | W2 native sea |
| 1ffbe66a | 45b62cb7 | Source branch default selection |
| a47e58a9 | 2fcfa4e5 | Historical W2 evidence |
| 2e252ac1 | b449e3d9 | W3a shoreline field and waves |
| 182f010a | 14c9407a | W3b shore simulation, wet sand and lace |
| a35f7c80 | 799a0b44 | W3c surf foam |
| 698d1e19 | 047e0de4 | W3d breaker lips |
| a2b9c135 | d44f5b41 | W3d.1 cached breaker stations/seaward side |

The final integration adaptation restores `waterBackend=0` (Current OP). A water smoke uses `WGR_WATER_BACKEND=1`; `0` explicitly forces Current OP. The Water tab can select either backend. An explicitly saved profile selection remains effective without an environment override, so testing the normal default requires a profile without a saved Tidewater selection or the explicit zero override. No source-series default flip is silently accepted.

The public C++/Rust ABI is bumped together from 16 to 17: the water parameter struct grows from 1056 to 1184 bytes and the GPU timer regions grow from 78 to 81. The previous paired render-call CPU timing API, pass facts, bounded geometry reports, section refresh and streaming paths are retained. A negative ABI16 test is added alongside existing older-ABI refusal tests. These tests are source-only until the root agent runs the supported build/checks.

Normal Current OP sessions do not retain a second full heightmap. The source series' eager shore replay copy is replaced with capture only when Tidewater is requested and supported. A later explicit switch borrows Terrain's existing sky-visibility source at the renderer frame boundary, copies it once for the shore backend, and clears it when switching back to Current OP. Cascade replay settings use a fixed inline array instead of an eager heap vector. Tidewater FFT/shore simulation, station caches and region tile work are created only after activation. Shore-lace derivatives are inside the uniform Tidewater selector branch rather than unconditional terrain fragment work.

The integration still has a backend wrapper and small replay settings, null/selection branches, and the terrain wetness binding layout with a 48-byte UBO and two 1x1 placeholder texture views. The terrain pipeline has an extra bind group. This is not a claim of zero default overhead or byte-identical renderer work. A matched normal-default screen is required before accepting performance; source inspection cannot establish it. Shader compilation must verify the uniform derivative branch, and the smoke must check startup selection, live switching, shoreline direction, breaker lips, wet sand, and resource behavior.

The copied `TW-WATER-*` decisions and measurements describe the source branch's historical observations, not results on the current streaming branch. W3 visual/performance acceptance and W4 spray remain open. Preserve the copied MIT notice in `CREDITS.md`, including DRG Software Solutions LLC copyright and Tidewater upstream pin `4811ba48d795197de5621985f404e765c0b7c0ef`. No licensed sea/foam assets are imported; the port uses procedural assets from the source implementation.

Root owns final integration against the newer streaming HEAD, all builds, deployment and game smoke. This isolated branch does not replace EngineWgpu files wholesale, launch a second game, deploy binaries, or promote Tidewater as the normal default. Streaming completion, whole-world compatibility, visual quality and performance acceptance remain open.

## Additional showcase branch

The subsequently requested exact safety ref `refs/heads/dec/tidewater-showcase` resolves to `cc27e21e086b91c5d766c89bf448d0197286651b`. It contains the same nine water commits plus one additional mission-only commit; patch-equivalence inspection marks the nine as already included. The additional source commit `cc27e21e` is preserved with `-x` as `eaa0ef73`. It adds `dev-missions/tw-showcase.eden/{README.md,description.ext,init.sqs,mission.sqm}` and changes no renderer code, ABI or normal backend default.

The showcase scripts six camera shots over an original Everon south-east bay landing with an LST, four PBRs, a platoon and aircraft. It ends itself at 120 seconds. This is source inspection of the authored timeline, not a verified mission execution or a water quality/performance result. The root agent owns installed-game verification, and should use the complete selected source checkout's mission directory explicitly if it is not copied by deployment.

After supported deployment of the matched pair, the root-owned water smoke can use this PowerShell launch from the game directory (replace the mission path with the final merged checkout's absolute path):

```powershell
$env:WGR_WATER_BACKEND = '1'
.\OpenPoseidon.exe --render=wgpu --window --width=1600 --height=900 --dev --test-mission 'C:\Users\mail\.codex\worktrees\tidewater-streaming-port\CWR-CE-paavohuhtalafork-\dev-missions\tw-showcase.eden'
```

A Current OP comparison sets the same environment variable to `0`; remove the session override afterwards. Game ownership/locking and supported deployment remain required. No launch was executed by the port subagent. All previous integration adaptations, including Current OP default zero, matched ABI17, lazy shore height capture and Tidewater-only shoreline derivative work, remain intact.
