# Unit loadout commands

`getUnitLoadout` captures an entity's weapons, magazine ammunition and selected
muzzle. `setUnitLoadout` replaces those values:

```sqf
savedLoadout = getUnitLoadout player;
player setUnitLoadout savedLoadout;
```

The returned array has three elements:

```text
[[weaponClass, ...], [magazineClass, ammoCount, magazineClass, ammoCount, ...], selectedMuzzle]
```

The magazine array is a flat sequence of class/count pairs. The setter accepts
the first two elements with an optional third string. It removes existing
weapons and magazines, adds the supplied magazines and weapons, reloads, then
selects the named muzzle when present. A negative ammunition count uses the
magazine type's maximum ammunition. This is the CWR-CE loadout format, not the
Arma 3 equipment-array format. It does not capture clothing or inventory grids.

## Integration and validation

Imported source commit `04145d371be5ae59dded612fb73a28a3ed6254bf` from
`jschmidt92/cwr_ce_source`, whose `feature/unit-loadout-commands` branch was
synchronized to `ef884f9757b80081fa6376e328e95eddf7accb05`.
OpenPoseidon's `feature/unit-loadout-commands` branch is based on `main` and
contains only the loadout feature. `feature/in-memory-database` contains the
database feature and its fixes. `feature/integrated-features` combines both.
See [CREDITS.md](../../CREDITS.md) for attribution.

The native implementation and tests compiled and linked with
`win-x64-clang-rwdi`. The build command then failed at the existing fixture-copy
step because this source release omits `tests/fixtures/font`.
Running the linked `PoseidonTests.exe` with filter
`[database],GameType constants*,VBS-derived functions remain registered in GGameState`
passed 645 assertions across 12 test cases on `feature/integrated-features`,
including registration of both loadout commands and the database regression
tests. `git diff --check` passed.

The game build and content staging also succeeded. The
[smoke-test mission](../../dev-missions/database-loadout.Intro/README.md) passed
on Windows with WGPU: a loadout was saved to disk, cleared, loaded into a new
database snapshot and restored with matching weapons, magazine ammunition,
grenades and selected muzzle. This runtime check required locally installed
proprietary Remastered assets; none are included in this contribution.
Multiplayer behavior was not tested.
