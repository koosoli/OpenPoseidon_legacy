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
  passed **99 assertions in 7 test cases**. This covers database persistence,
  list/hash mutations, handle defaults, the data factory, type constants and
  command registration. The full asset-dependent suite was not run.
- `git diff --check` passed. Source comparison confirmed exact matches for the
  imported implementation, header, tests and guide; command registration merged
  with OpenPoseidon's existing commands. All 21 database commands occur once.

## Findings in the imported implementation

These findings remain present in the source feature and this faithful port.

- **High: saving can lose the previous database.** `DatabaseSave` removes the
  destination before renaming the temporary file. A crash between those
  operations, or a failed rename, leaves no previous database. The failure path
  also deletes the temporary file and overwrites the original error code during
  cleanup. Use an operating-system replacement operation that preserves the
  previous file if replacement fails, and retain the original failure code.
- **Medium: released databases retain memory.** `ReleaseDatabaseRef` decrements
  the reference count without clearing the root; `NewDatabaseHandle` always
  appends another entry. Repeated creation or loading retains all database
  contents until process exit. Reclaim contents when the last reference is
  released and use a lifetime-safe strategy for recycling entry slots.
- **Medium: list indices are converted before validation.** List get/set/remove
  cast script floats to `int`; pop casts to `size_t`. Non-finite or out-of-range
  values can cause undefined floating-to-integer conversions. Negative fractional
  indices can truncate to zero in get/set/remove. Check finiteness, integrality
  and range before conversion, with regression coverage for each operation.

The port preserves upstream behavior so that these corrections can be reviewed
separately from the import. Existing tests cover ordinary persistence and list/
hash operations; they do not establish correctness for these failure cases.
