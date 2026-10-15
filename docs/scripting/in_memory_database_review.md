# In-memory database integration review

## Source and integration

Imported `ed938b183cd5af91453b8df9b3236add57cd9e7f` from
`jschmidt92/cwr_ce_source` onto OpenPoseidon `cc8b9f4` using a cherry-pick with
source attribution. The source feature branch was synchronized to `96bda9c`.
Its feature-only diff against `origin/main` contains the same five files as the
original feature commit; subsequent merges bring upstream engine updates.

The import includes the database value type, 21 scripting commands, binary
persistence, scripting documentation and four database-specific test cases.
Existing type and command-registration tests are extended as well. CMake's
recursive Game source collection includes the new implementation, and the
existing `PoseidonTests` target includes the updated test source.

## Validation

- CMake configuration with `win-x64-clang-rwdi` passed.
- C++20 syntax checks passed for the implementation, command registration and
  updated test source against OpenPoseidon headers.
- The native `PoseidonTests` executable compiled and linked. The target's
  subsequent fixture-copy step failed because `tests/fixtures/font` is omitted
  from this source release (see [BUILDING.md](../../BUILDING.md)). The overall
  build command therefore returned failure, despite producing the executable.
- Running that executable with filter
  `[database],GameType constants*,VBS-derived functions remain registered in GGameState`
  passed **645 assertions in 12 test cases** on `feature/integrated-features`
  after all three fixes and the CLI11 startup correction. This covers database
  persistence, replacement failures, handle lifetime and slot reuse, list-index
  validation, list/hash mutations, type constants and command registration,
  including the two loadout commands. The full asset-dependent suite was not run.
- The game build and content staging succeeded. The
  [smoke-test mission](../../dev-missions/database-loadout.Intro/README.md)
  passed on Windows with WGPU: the player's loadout was saved to disk, cleared,
  reloaded and restored with matching weapons, ammunition, grenades and selected
  muzzle. This check required locally installed proprietary Remastered assets;
  no such assets are included in the contribution. Multiplayer was not tested.
- `git diff --check` passed. The original import matched the upstream feature;
  the fixes below intentionally change that implementation. All 21 database
  commands remain registered once.

## Findings in the imported implementation

All three findings below are fixed on this branch.

- **Fixed — High: saving could lose the previous database.** `DatabaseSave` removed the
  destination before renaming the temporary file. A crash between those
  operations, or a failed rename, leaves no previous database. The failure path
  also deletes the temporary file and overwrites the original error code during
  cleanup. It now uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` on Windows
  and rename on POSIX, without deleting the destination first. Cleanup uses a
  separate error code. Regression tests cover a destination directory remaining
  intact and, on Windows, a blocked replacement preserving the old data followed
  by a successful retry. The existing test covers successful initial saves and
  overwrites. Power-loss durability is not guaranteed.
- **Fixed — Medium: released databases retained memory.** `ReleaseDatabaseRef`
  now resets the root when the last reference is released, freeing nested
  values and container capacity. An intrusive free list reuses empty slots
  without allocating during release. Copies and clones retain references, so
  their slots cannot be reused while they are live. Slot-table capacity remains
  available for peak simultaneous usage. Tests cover copies, clones,
  self-assignment, replacement assignment, empty reused slots, preservation of
  other live databases and repeated creation/loading. Both new handle-lifetime
  tests failed against the previous implementation and passed with the fix.
- **Fixed — Medium: list indices were converted before validation.** All four
  indexed list operations now share a validator that checks scalar type,
  finiteness, integrality and bounds before converting to `size_t`. Invalid
  indices preserve the list and return the existing fallback/failure result.
  Regression coverage includes negative fractions, positive fractions, NaN,
  both infinities, extreme floats, integer-conversion boundaries, nonnumeric
  values, empty/missing/non-list keys and valid boundary operations. The new
  regression test fails against the previous implementation because `-0.5`
  reads the first element instead of returning the fallback.

The isolated `feature/in-memory-database` branch passed 643 assertions in 12
test cases after these fixes. The integrated branch adds two loadout-command
registration assertions, producing the 645-assertion result above.
The POSIX replacement path has not been runtime-tested here.
