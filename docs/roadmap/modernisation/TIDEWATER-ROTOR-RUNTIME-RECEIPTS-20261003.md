# Installed rotor-water comparison and copied-buffer receipts

## Installed bounded result, 3 October

Matching bd590703 campaign `supported-stock-nogova-sea-20261003-131046-1d2abf`
completes all three arms with normal exit0 and unchanged installed EXE/DLL.
Actual occupied UH60 physics, native Noe sea support, pause/camera return, high
and dry controls, and normal Z landing/shutdown gates pass. Tidewater ON low
hover copies the actual rotor descriptor; OFF, high, dry and stopped sea
controls copy zero sources with the current clock and camera domain.

Root inspected low top/close PNGs. Existing OP has its clearly visible white
foam ring over Nogova. Tidewater ON has a visible concentric ripple field in the
top view; the matching OFF view has no rotor ring. The low close-view disturbance
is subtler against strong sun glare. This establishes bounded source admission
and a visible enabled effect over this native sea location, not every map,
camera angle, multi-aircraft capacity or acceptable cost. No GPU cost was
measured by this fixture. Source-only pending notes below are historical.

The Tidewater rotor bridge requires an installed runtime comparison. CPU policy
and WGSL composition checks establish admission, layout and bounded behavior;
they do not establish a visible ring or its GPU cost. The new runner captures
those unresolved questions without injecting a water event or forcing rotor RPM.

## Actual renderer receipt

`WGR_TW_ROTOR_TRACE=1` enables a `TW_ROTOR_UBO` JSON line on stderr immediately
after the actual Tidewater surface uniform buffer is written. Unset or any value
other than exact `1` disables it. Each line contains the copied water clock,
rotor phase clock, four domain lanes, source count and all admitted descriptors,
up to eight. It does not change a public ABI or use a game harness callback.

Identical per-frame state remains quiet. A line is allowed after one simulation
second, a backwards clock transition, an actual source-count transition or an
actual domain transition. The last rule permits inspecting a different render
camera while simulation is paused. Disabled rotor wash keeps current empty
domain/clock metadata and clears all sources; its zero-source material remains
unchanged. This gives the fresh restarted OFF arm an honest copied-buffer
receipt rather than stale default metadata.

## Runner contract

`scripts/Test-RotorWaterRuntime.ps1` runs three fresh game processes by default:
Current OP, Tidewater with rotor wash disabled, and Tidewater with rotor wash
enabled. Each uses its own profile, captured engine log and stderr. A required
`-ExpectedCommit` checks the installed deployment stamp, and executable/DLL
hashes and modification times must remain identical across all arms. Run it
through `scripts/with-game-lock.sh`; a foreign game is preserved and invalidates
the comparison. Only the process launched by this runner can be cleaned up.

The mission is stock `perf_sand.noe`. Its actual US soldier boards a stock UH60
as the controlled driver. Actual physics spool the rotor on dry ground. The
existing physical auto-hover receives one relocation per stage and an ordinary
brief collective input to refresh its height helper. There is no velocity/RPM
pinning, artificial projectile, rotor event submission or freefly pilot.

The audited sea location is X 1750, Z 3100. Twenty-five native bathymetry samples
within 32 m must remain underwater against the current sea datum. Dry support
at X 2475, Z 5075 is checked against the same datum. Stages are low sea hover,
paused captures, high sea hover above the admission window, return to low sea,
dry hover and a supported engine-off spool-down followed by a sea-view capture.
All hover stages require actual local player/driver identity, live manual
authority, auto-hover, healthy rotor RPM, bounded location and safe altitude.

Close and top captures use `triSetView`, the existing render-only camera lease,
while physics is paused. A physical snapshot before and after must retain its
clock, position and RPM. Tidewater receipts must match the actual snapshot's
clock, source position, RPM-derived radius/strength and the camera's actual
256 m snapped domain. High, dry, engine-off and disabled arms must copy zero
sources. A successful positive low-sea arm must copy its actual aircraft source.
Current OP supplies physical admission evidence and real captures only; the
runner does not claim to receipt a copied legacy interaction buffer.

## Use and outstanding acceptance

Source-only schema checks need no game:

```powershell
pwsh -NoProfile -File scripts/Test-RotorWaterRuntime.ps1 -SelfTest
pwsh -NoProfile -File scripts/Test-TidewaterRotorPolicy.ps1 -Compiler 'd:/Program Files/LLVM/bin/clang++.exe'
```

After the build/deploy owner installs the commit containing these changes, run
from Git Bash with the actual installed commit substituted:

```bash
LOCK_OWNER="stock sea rotor comparison" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-RotorWaterRuntime.ps1 \
  -ExpectedCommit ACTUAL_INSTALLED_COMMIT -Label stock-sea-rotor
```

For a targeted rerun, `-Arms TidewaterOn` selects one fresh process. Full visual
acceptance still requires reviewing both close/top enabled images against the
fresh disabled images and the Current OP reference. The result status explicitly
leaves appearance and cost pending. No installed execution or visual acceptance
is claimed by the source-only runner commit.
