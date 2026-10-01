# Retain rotor authority and RPM facts around the existing flight fixture

Baseline `50db754c`, including the additive read-only `rotor_state` command.
Only `scripts/Test-RotorLandRuntime.ps1` and this document change. The initial
fixture deliberately retains the same boarding, `doStop`, engine-start,
18 m relocation, quarter-second Q tap, normal EXTERNAL view and existing hover
requirements. No grounded spin-up recipe, source physics, forced airborne state,
velocity pin or RPM override is introduced by this diagnostic slice.

The runner now retains snapshots at startup after boarding and after the view,
before and after engine start, before each Place, immediately after relocation,
after Q press while paused, while Q is held before release, after release and
pause, and after the resulting view. These facts distinguish an engine command
that never became active from a demand that is revoked at the first simulation
tick, and an injected Q key from a filtered or suspended collective input.

Each `AwaitHover` poll records the actual aircraft position, `isEngineOn` and
full read-only state together. Its existing 60 s deadline and 500 ms sleep
bound the failed poll history. Ordinary observation stages additionally sample
state before/after and at approximately two-simulation-second milestones.
The existing land particle, RPM, hover, pause, weather and coverage acceptance
thresholds remain unchanged. Wrong engine or authority state is recorded,
not coerced into a passing state or rejected before it can be inspected.

Every snapshot is copied into `result.json` and appended immediately to
`rotor-states.jsonl`, with a stage label, actual `timeMs` and wall UTC. The
existing serial harness transcript also retains the exact request/reply. The
query uses current actual X/Z and a 16 m nearest radius, establishes one stable
RenderId across the campaign, validates the UH60 identity, required typed
fields and finite values, and fails if the diagnostic silently switches objects.
No additional hypothetical authority predicate is inferred from debug names.

Read-only diagnostics still have IPC/frame latency while simulation is active.
In particular, the Q-held snapshot adds a query round trip before key release;
the retained simulation timestamps are authoritative for the measured hold,
not a claim of an exactly 250 ms simulated pulse. This is the same control
recipe instrumented for diagnosis, not a bitwise timing comparison or a new
spin-up fix. Paused press/release snapshots may also precede consumption of a
queued SDL event; compare the held, active-simulation snapshot before deciding
that Q is unavailable.

For the first diagnostic run, use the same failing fixture parameters under
the shared lock, for example:

```powershell
bash scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-RotorLandRuntime.ps1 -ManualPilot -SandX 2700 -SandZ 5150 -Label actual-rotor-authority
```

Keep a failed hover as failure and inspect `engine-start-paused`,
`place-18-q-held-before-release` and the following `await-hover-poll` records.
Facts of interest are actual RPM versus engine demand, fuel/total damage,
driver/pilot/player equality, manual flags, player suspension/camera effect,
active/resolved input context, focus-filtered and unfiltered MoveUp, Q state,
brain state, altitude/vertical speed, local airflow and camera distance.
The read-only query cannot expose the private engine-hit channel or certify
visual-effects precision/particle readiness. Those remain follow-up source
diagnosis if the basic flight/input gates prove sound.

Only after these facts are installed should a separate grounded real-spin-up
recipe be considered. Source limits RPM rise to 0.05 per simulation second,
so at least thirteen simulated seconds are necessary from zero to 0.65. That
does not prove engine demand persists or establish an actual hover. Existing
accepted ocean downwash is untouched.

Author checks: PowerShell parser returns zero errors; `-SelfTest` passes the
actual response schema and missing/nonnumeric/nonfinite/incorrect-source
falsifiers while allowing a valid snapshot with engine off and wrong authority.
`git diff --check` passes. No game, GPU check, deployment or full build was
performed. Root owns the installed serial diagnostic and evidence reconciliation.
