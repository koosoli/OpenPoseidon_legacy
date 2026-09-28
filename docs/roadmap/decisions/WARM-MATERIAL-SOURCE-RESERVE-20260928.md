# Two archive-source binding slots reserved for opt-in material preflight

Source 89a9d9c6 changes the existing optional warm source/job/material-preflight combination. While those three exact flags are enabled, ordinary model reads stop acquiring new bindings at 254, leaving up to two of the unchanged global 256 bindings available to owner `MaterialPreflight` reads. The same 2-MiB metadata budget remains authoritative, and the bank still returns the original bytes if optional source capture fails. This is a purpose-aware admission limit, not a new cache, source freshness certificate, worker slot or upload guarantee. With the optional combination OFF, the ordinary 256-binding rule remains. The extra owner-intent/limit checks have a nonzero cost in the opt-in path; no default performance benefit is established.

All five targets built in 211 scheduled steps. Fresh tests with their correct environment passed without skips: actual PBO/source reserve ON 541 assertions/one case, OFF 525/one, and model-purpose plus warm-preflight 68/four. Supported installed stamp 89a9d9c6 at 02:02 matches executable SHA-256 92E15749B3A7C59E92D7EA84A6D8CBD3A8FC44275A4A932DD1B03A6F4EFDD002 and renderer DLL 011B82EC875F807E505F65B6E911E5CFB88CA46035FA7F78CCA8E97E7D56D586. These scoped tests establish reservation arithmetic and unchanged original-read fallback, not a real DayZ stage upload.

Two root-owned DayZ runs on the same installed pair both failed the unchanged strict bound worker normal/specular filename-family upload criterion, with owned exit 0, normal shutdown and no forced termination. In `warm-stage-reserve-20260928-disabled-20260928-020310`, two observed material-stage sources were usable, had current archive bindings, and were accepted; their exact-member tokens entered the prepared store and were later retired as Cancelled. The final proof still has no same-source stage upload. Final aggregate store counters include five Puts, one Take, one successful 1,398,016-byte *primary* upload and four prepared-PAA cancellations; these aggregates are not two stage-upload successes or a full per-token count.

In `warm-stage-reserve-reuse-20260928-disabled-20260928-020622`, two observed stage sources again had accepted current bindings and StorePut rows. Two historical exact-member `storePromotedPublished` rows followed. They show that the optional pre-cancellation transfer operated, not that either stage entry was consumed: stage Takes/uploads remained zero, `sameSourceStageUploads=[]`, and the strict result remained false. The only successful prepared upload was again the separate 1,398,016-byte primary path. Final aggregate store counters were three Puts, one Take, one upload, zero prepared-PAA cancellations and two retained entries/6,293,664 known store bytes. Promotion trace tokens identify key plus physical member, not an entry incarnation or later upload. Other `_sm` candidates still reported `NoInitializedBinding` with global capacity-refusal deltas; those global counters do not establish the cause for each texture.

The runs had 867 and 850 frames, both with zero drops. Means were 40.886 and 40.556 ms, medians 23.665 and 24.456 ms, nearest-rank p95 119.901 and 127.401 ms, p99 354.314 and 336.279 ms, and maxima 1536.219 and 904.944 ms, respectively. Different admission/capture/async work and ordered cache state prevent a causal FPS or no-regression comparison. There is no material-stage readiness, all-texture, GPU completion, pixel/all-pass, physical-memory-release or default promotion claim. The required experimental flags remain OFF by default; whole streaming plan remains OPEN, `defaultReady=false` and `performanceAccepted=false`.

Evidence SHA-256 manifest: `build/warm-stage-reserve-artifact-manifest-20260928.json`.

The DayZ road-stage sources accepted in these routes use the `_sm.paa` suffix.
`ShaderSchema` classifies that suffix as SpecularDetail, but the runner's
historical filename-family filter excluded it. The runner now includes `_sm`
for future witnesses. Reclassifying the filename does not change either
recorded failure: neither run had a stage Take or upload. These roads also
had no observed retained GPU-owned specular consumer after their preparation.
Future proof needs a visible GPU-owned model with the same prepared stage
source, rather than an upload created solely to satisfy the harness.
