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
OpenPoseidon's `feature/unit-loadout-commands` branch builds on the existing
in-memory database integration. See [CREDITS.md](../../CREDITS.md) for attribution.

The native implementation and tests compiled and linked with
`win-x64-clang-rwdi`. The build command then failed at the existing fixture-copy
step because this source release omits `tests/fixtures/font`.
Running the linked `PoseidonTests.exe` with filter
`[database],GameType constants*,VBS-derived functions remain registered in GGameState`
passed 101 assertions across 7 test cases, including registration of both
loadout commands and the database regression tests. This does not verify
in-game loadout round-trips or multiplayer behavior. `git diff --check` passed.
