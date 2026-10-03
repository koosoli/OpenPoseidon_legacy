# Occupied flight staging lost its held freelook key

`combat-audit/build/rain-helicopter/occupied-rain-solver-attribution-20261003-135005-827584`
ended normally with exit zero and an unchanged installed `e017e028` EXE/DLL
pair, but failed before any measured route. It is not an accepted paired solver
attribution result. The unchanged script hash matches the earlier successful
`occupied-matched-rain-weather-20261003-132025-8b38bb` campaign.

The actual failed predicate is `Assert-Pilot`'s existing freelook/focus/joystick
authority guard. The helicopter retains Alt=1 and `lookAroundEnabled=true`
through simulation time 41004 ms; at 42075 ms both become zero/false and remain
so through the final 76245 ms sample. The engine logs an actual window focus
loss at 13:51:08.037. This association does not identify every SDL event that
cleared the key. Harness input records contain one Alt-down during initial
staging and its only explicit Alt-up at teardown; there was no normal stock
key readmission after the loss.

The final actual helicopter remains local, occupied by the live manual player,
healthy, engine on, RPM 1, airborne, clearance 122.651 m and almost stationary
in XZ. Vertical speed is 0.02062 m/s, actual driver focus and ordinary external
camera remain present, the input focus-settle counter is zero and both joystick
flags are false. Final automatic liquid rain is 6.7868345e-16. Replaying those
actual weather/particle receipts through the unchanged `Assert-Weather` passes.
Actual `Assert-Pilot` fails specifically with
`Unexpected focus/mouse/joystick authority.` An offline copy with only the lost
freelook Boolean changed isolates that predicate; no modified state is used to
admit a runtime sample.

The runner now records every readiness attempt's actual helicopter state,
weather, particle parameters, rejection reason and whether stock Alt input was
readmitted. On that exact authority failure only, it may send ordinary
`key sc=226 hold=true` at most twice while actual Alt is zero, look and toggle
are false, the focus-settle counter is zero and no joystick is active. A fresh
later state must satisfy all existing pilot/weather guards. The existing
30-second readiness deadline and every measurement guard are unchanged.
Measurement never refreshes lost keys; a loss there still refuses the result.
Held-key bookkeeping deduplicates the cleanup list so a repeated admission
still receives one normal teardown release.

`Test-RainHelicopterPerformance.ps1 -SelfTest` passes without a game/device.
It refuses unbounded repair, existing held Alt, active focus settling, look
toggle, joystick authority and malformed actual key data, and still refuses
lost freelook through the normal pilot validator. A whole installed paired
runtime campaign remains pending; the failed campaign's partial dry Advance
cost is not evidence of a rain-vs-dry improvement.
