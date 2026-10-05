# Normal Options view-distance precision, 2026-10-05

Normal Options -> Game distributed100..50000m over100 slider positions, and
shared notebook input advanced5 positions per arrow/click and quantized drag
into20 positions. Thus700m jumps to3100m;800m is unavailable through these steps.
Actual installed248df7ad reproduction, isolated700m profile and SDL Down/Right
input: build/view-distance-options/20261005-185418/result.json (expected failure;
recorded afterRight3100m, normal cleanup).

Game view-distance positions now represent100m each (0..499), arrows step one
position. RowDef carries optional maximum/step with100/5 defaults for existing
percentage sliders; shared input and fill normalization honor the actual row.
No settings-file format, mission-visibility policy, maximum or renderer change.

Six GamePage tests pass,1028 assertions, including every supported100m value,
actual WithCloseRow provider delegation, range clamps,800m text and unchanged
percentage defaults. Full game build and installed native-menu persistence
smoke test pending. scripts/Test-ViewDistanceOptions.ps1 uses actual SDL key
input to select800m,900m,800m, close/save and reopen the page; isolated user dir.
