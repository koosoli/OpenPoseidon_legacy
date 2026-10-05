# Local swim normal-launch acceptance, 2026-10-05

Installed source c3ee325c (18:58). The pre-existing local swim archive did not load
without the optional launcher's --mod argument. The identical SHA256
B83EB8C119FE0D2174C06EA0F5EA68B3696045F4D7157715C3403E8D4B004EBB archive is now
installed locally at the game AddOns/op_swim_local_test.pbo path. Original game
archives are unchanged; no animation/config bytes are integrated into Git.
The local test launcher avoids loading this archive twice and uses the recovered
worktree's game lock. Its previous worktree disappeared during testing.

Repeated installed acceptance with no --mod argument passed: actual SDL W six
seconds selected swimf and travelled 5.34m, release returned to treading, and
scripted relocation to land restored combat movement. Stroke screenshot inspected.
Normal exit0, unchanged EXE/DLL hashes, no GPU/access/cache-unlock failures.
This does not qualify every stroke, AI, multiplayer, natural shore traversal or
complete Arma animation compatibility. Receipt: local-swim-autoload-20261005-runtime.json.

The reported third-person own-player invisibility on land remains OPEN. Source
audit found player CPU occlusion exemptions and external-camera routing present.
A SoldierWB on stock Everon was visible in seven unobstructed six-metre orbit
views; the eighth camera was behind a tree. Four close 2.5m cardinal views also
showed the soldier. Real SDL walking selected combatrunf and moved normally.
The orbit uses native EXTERNAL camera type with a pinned render view; it does not
prove all native camera controls, angles, classes, or multi-unit shared poses.
No speculative renderer fix has been made or claimed. The installed build was
compiled without DLSS, so its DLSS path is not implicated by this test.
