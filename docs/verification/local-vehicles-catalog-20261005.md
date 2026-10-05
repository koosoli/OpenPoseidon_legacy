# Local Arma vehicle bridge catalog

The local-content browser can activate existing CWA vehicle bridge addons and
open a native OFP preview mission. It does not import a foreign game's
`CfgVehicles`, replace stock classes, copy source assets into Git or promise
universal vehicle compatibility.

The first provider discovers `@cwr_t72_a1` beside the game or under its `Mods`
directory, case-insensitively. Its authored config maps selected later-game
models to OFP simulation classes. Before enabling a row, the provider requires
the complete eleven-archive dependency pack, a declaration of the class in the
small local bridge config, and the selected model's actual uncompressed PBO
entry, virtual prefix and ODOL revision. Missing, compressed, incorrectly
mounted or different-revision models remain disabled with an explanation.

| Browser row | Local class | Checked model revision | Simulation |
|---|---|---|---|
| Arma 1 T-72 | `CWR_T72_A1` | ODOL 40 | OFP T72 |
| Arma 2 T-72 | `CWR_T72_A2` | ODOL 48 | OFP T72 |
| Arma 3 T-100 Varsuk | `CWR_T100_A3` | ODOL 73 | OFP T72 |
| Arma 1 UH-60 | `CWR_UH60_A1` | ODOL 40 | OFP UH60 |
| Arma 2 Mi-35 | `CWR_MI35_A2` | ODOL 48 | OFP Mi24 |
| Arma 3 MH-9 | `CWR_MH9_A3` | ODOL 73 | OFP UH60 |

The six rows were discovered and passed metadata checks against the owner's
real local pack on October 5. The provider compiled independently with Clang;
three Catch2 cases passed thirty assertions covering revision and namespace
checks, compressed/missing/truncated models, incomplete addon packs and safe
preview mission generation. Native browser interaction and installed-game
preview verification are still pending integration by the parent session.

Existing engine evidence for the bridge is recorded in
`docs/roadmap/decisions/AST-016B-proxies-and-the-cross-generation-tank-bridge-20260825.md`
and `AST-016C-five-helicopters-board-the-cross-generation-bridge-20260825.md`.
Those documents establish selected fixtures, not every vehicle. Current
compatibility includes narrow ODOL 40/48/49/50/52/54/73 readers, proxy adapters
and partial selection/skeleton adaptation. Native Arma simulation, scripts,
modern nested turret behavior, PhysX handling, all animation sources and all
vehicle materials are not supplied by mounting a readable model. Additional
vehicles need individually authored bridge classes and dependency checks.

Raw Arma addon config mounting is deliberately avoided: prior measured gameplay
tests found that it redefined OFP classes and prevented mission startup. Local
foreign models/textures belong in config-free source mounts or existing
config-free bridge PBOs; only the authored CWA bridge config is activated.
