# Local swimming test installation, 2026-10-04

The owner explicitly requested local installation without integrating the held
animation assets into our Git branch. Installed a separate opt-in addon at
`C:/Users/mail/AppData/Local/OpenPoseidon/LocalMods/@OP_SwimLocalTest`.
No animation assets, addon configuration, packaging script or launcher are added
to our integration branch. This document records the local installation only;
the redistribution/provenance hold is unchanged.

Source: the exact audited Dec tip `f758e9f3b1008b3bfd1052a0fccaf24a6965e3e1`,
19 RTMs plus the existing swim configuration. Binary extraction preserved bytes.
Archive: `AddOns/swim.pbo`, mount prefix `swim`, with all RTMs at archive root.
SHA-256: `b83eb8c119fe0d2174c06ea0f5ea68b3696045f4d7157715c3403e8d4b004ebb`.
Installed engine stamp remains `f8127fe5` from 11:41; no binaries replaced.

Offline checks passed:

- All 19 animation byte counts and hashes match the prior Dec audit.
- Complete RTM layouts, ordered finite phases, unique bone names and finite matrices.
- All configuration animation references resolve to included files; config parses to binary.
- Purpose-built PoseidonTools pack/extract roundtrip preserved all 20 archive files.
- Installed local files match staged copies; launcher parses as PowerShell.

Local `Launch-Swim.ps1` defaults to the player-controlled bay mission. Add
`-Mission Squad` for the AI camera demonstration. It invokes the existing game
lock, refuses concurrent games, uses the installed engine, and enables swimming
only for that launch. It never changes persistent mod activation or default
content registration. Logs and the full installation receipt live in the addon.
Both missions were copied from the audited source tip.

For another mission, add `--mod` with the absolute local addon directory to the
normal command. Normal launches without this argument keep the addon disabled.
Bay controls: W forward, E faster, release to tread water; swim back toward the
beach to stand. Sideways/backward/diagonal strokes and turns are also included.

At installation time the owner demo PID 5104 was preserved. The owner then
explicitly authorised closing it. Runtime testing used paired installed build
**bdba184a, deployed 12:57**, with the same local addon archive hash.

Installed checks passed: player treads water; actual SDL W held six seconds
selected `swimf` and moved **4.719 m**; release returned to `swim`; scripted
dry-land relocation returned to `combat` at native terrain elevation 42.03 m.
Normal exit code 0 and no GPU, access or cache-unlock error. Tread/stroke/land
screenshots were captured; the stroke image was visually inspected.

The first attempt exposed a real source defect: clearing `_swimming` after
lifting/relocating above the sea left the looping swim action map active.
bdba184a makes both shallow and lifted exits restore upright land movement;
lifted exits return to the normal default animation, allowing the existing
falling path to take over when needed. Animation assets remain local-only.

This is bounded player acceptance, not a test of all 19 moves, first-person
view, AI boats/squad, gear buoyancy, natural continuous shore traversal or
full animation appearance. The AI mission is installed but was not run.
Final receipt: `docs/verification/swim-cache-installed-20261004-runtime.json`.
The passing swim arm comes from a combined run whose later stress probe
failed on unsupported script syntax; corrected stress-only rerun passed with
the identical installed binary hashes. No whole-run success is inferred from
that partial run. All owned smoke processes were closed normally.
