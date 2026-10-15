# Database and loadout smoke test

Launch a built game with compatible CWA/Remastered data and these options:

```text
--window --test-mission <repository>/dev-missions/database-loadout.Intro --log-file <test-log>
```

Use `POSEIDON_USER_DIR`, `POSEIDON_CACHE_DIR` and `POSEIDON_TEMP_DIR` to select
isolated test directories. The mission writes `integrated-loadout-test.db` in
the test profile's `Config` directory and overwrites it on subsequent runs.

The script captures the player's loadout, stores it in a database, saves it,
clears the weapons and magazines, loads a separate database snapshot, and
restores the saved loadout. It compares all three returned loadout fields and
reports `INTEGRATED_TEST: PASS` or `INTEGRATED_TEST: FAIL` in the log and an
in-game hint. On failure it attempts to restore the original loadout.
The mission stays open for manual movement and firing tests.

Validated on Windows with WGPU and local Remastered data: the M16, magazine
ammunition, grenades and selected muzzle matched after the round trip.
This is a single-player smoke test, not multiplayer coverage.
