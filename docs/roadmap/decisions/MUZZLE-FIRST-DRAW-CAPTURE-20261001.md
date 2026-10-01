# Capture the real visible muzzle flash on its first draw

## Why this slice exists

The existing muzzle illumination fixture waits 500 ms before each screenshot.
The rifle's visible flash uses UI time, with a 50 ms interval, while the new
illumination pulse uses simulation time. Pausing in the actual Fired event can
retain the illumination for a useful comparison; it cannot freeze the UI-time
sprite. Texture remap/material telemetry consequently proves source admission,
but does not establish that the screenshot contains a visible flash.

## Source and ownership

This candidate starts at c666e357. It changes only the test-command implementation
and registration, the installed muzzle runner, and this decision record. Root
owns the combined build, installed game lock, deployment and runtime review.

`TriScreenshotNext` accepts 1–80 ASCII alphanumeric characters, uses the existing
`TRI_OUTPUT_DIR`, creates an absolute normalized output directory, shares the
capture sequence counter and rejects an existing PNG/BMP prefix. It returns
`OK:<actual absolute extensionless prefix>` or a failure string. The command is
registered under the existing developer/harness/test-mission registration gate.
It calls `GEngine->Screenshot` without `FlushPendingScreenshot`. Queue telemetry
records the actual prefix and UI time; neither flash duration nor game simulation
changes.

The actual Man firing route arms `_mGunFireTime` and `_mGunFireFrames=1` before
calling the base effect route that invokes `EEFired`. The renderer's screenshot
request clears preceding screenshot pixels, requests a copy in the next acquired
render frame, and its normal NextFrame flush writes that frame. The existing
one-draw flash guard remains intact. This establishes the intended capture route,
not a guarantee that every screenshot exposes unobstructed muzzle pixels; shadow
passes, timing, view direction and the actual model must be checked at runtime.

## Installed runner

Optional `-CaptureVisibleFlash body|frontBody|surfaces` keeps the ordinary real
M16/SDL mouse input, exactly one Fired event and one round decrement. It applies
`triSetView` before the shot and asserts that no camera effect is active. The
Fired handler queues a unique alphanumeric label, records the verb reply, then
uses the existing pause. The runner uses the returned prefix rather than guessing
the shared sequence number, requires one matching queue trace and a valid hashed
PNG, and restores `triClearView` before the existing illumination views.

The same fixed render-only view supplies `visible-before`, `visible-first-draw`
and `visible-expired`. Expiry advances actual simulation by at least 200 ms
(at 0.02 time compression, well beyond the 50 ms UI interval), verifies no extra
shot and captures the same view again. The snapshot itself may cause a GPU
readback stall; it is diagnostic and unsuitable for performance measurements.
Recoil and random stock flash phase can differ between shots/runs, so neither
bitwise equality nor a global image difference proves visual quality.

Suggested first installed command, wrapped in the root's game lock:

```powershell
pwsh -NoProfile -File scripts/Test-MuzzleLightRuntime.ps1 -Scenario Night -Arm On -CaptureVisibleFlash frontBody -Label firstdraw
```

Repeat with `-OriginalFlash` for the accepted stock appearance control using the
same installed pair. Inspect actual muzzle pixels and expiry before declaring
the visual flash accepted. The `Arm` switch controls local illumination, while
`OriginalFlash` controls the visible texture/material appearance; these are
separate comparisons. Roof and other requested view cases remain available.

## Verification and remaining work

- PowerShell parser: pass, zero syntax diagnostics.
- `Test-MuzzleLightRuntime.ps1 -SelfTest`: pass. Includes real fixture coordinate
  arithmetic, engine-axis conversion/direction, refused/outside/mislabeled queue
  paths, unsafe labels, existing event age/owner and evaluator decoder checks.
- The actual C++ label predicate was extracted from `TriScreenshotNext`, compiled
  with clang++ C++20 `-Wall -Wextra -Werror`, and executed: empty label, lengths
  80/81, valid mixed ASCII alphanumeric, every non-NUL byte 1–255, traversal and
  spaces behaved correctly. Temporary test artifacts are under the isolated
  worktree's ignored `build/muzzle-first-draw-cpu`.
- Source contract confirms the new verb contains no synchronous flush; existing
  flash lifetime/gameplay files are unchanged. `git diff --check`: pass.

No engine build, GPU test, game launch or deployment was performed by this agent.
Root must compile the complete command translation units and run the installed
fixture. Functional queue/PNG gates deliberately retain **visual review required**:
only the actual next-draw captures can establish visible flash readability and
expiry. All existing private-profile, provenance/hash, serial ownership and
normal-exit checks remain in effect.
