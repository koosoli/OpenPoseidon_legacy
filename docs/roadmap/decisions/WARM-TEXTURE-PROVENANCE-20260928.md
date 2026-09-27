# Warm prepared-texture provenance: bounded strict failure

Source 893b48b9 adds a numeric, opt-in eight-member / 128-row trace. It copies Windows archive file ID, volume,
archive size and member offset/length; the same tuple is used by `SameArchiveMember`. It retains no extra source
lease or path. A 9184-byte known-C++ charge shares the existing 2 MiB archive metadata budget. The log is a prefix
with at most four stamps per event and source, not a full-route count. `StoreTakeMissKeyOnly` deliberately says the
miss was linked by an unambiguous key, not by a live store entry's physical identity. A positive GPU handle means
the owner create call returned a handle; it is not GPU completion or rendered-pixel evidence.

All five targets built in 602 scheduled steps. Focused trace tests passed 44 assertions / four cases; weak-cache
ON passed 80/two and OFF passed 23/one, without skips. The first focused unit runner omitted the hot-retirement
prerequisite and failed two cases; its observation is retained separately. The corrected runner used fresh processes.
Supported installed stamp ed40abd4 at 00:34 compiled the unchanged 893b48b9 source.

The actual DayZ run `build/stream-residency/warm-provenance-on-20260928-disabled-20260928-003441` failed the
unchanged strict later source-validated warm upload criterion. `result.json` records passed=false; `failure.json`
retains owned exit code 0, no forced termination and a complete shutdown. The trace has two exact physical tokens:

- Token 1 was admitted/submitted at 00:35:15.230. Owner exact attempt and key-only store miss occurred at 15.336,
  followed by ordinary 1,398,016-byte upload at 15.340. Worker start and StorePut/WorkerPut followed at 15.777/15.780.
  The prepared result was too late for that owner attempt.
- Token 2 was admitted/submitted at 00:35:19.296. Worker start at 19.298 and StorePut/WorkerPut at 19.325 preceded
  owner exact attempt and key-only store miss at 22.113; ordinary 5,592,064-byte upload followed at 22.133. Thus
  its preparation was early but unavailable at Take. No token-specific erasure event or reason is captured. The
  aggregate prepared-PAA cancellation count is one, but cannot be assigned to token 2. With no TTL override, the
  default 30-second TTL alone does not explain a roughly 2.79-second gap. Reset, cancellation, capacity retirement,
  or another lifecycle transition require a separate keyed diagnostic before attribution.

Aggregate worker jobs submitted/completed 2/2, warm Puts2, warm Takes0, warm Uploads0, final store one entry and
1,573,776-byte capacity. Final archive proof: strong256, known C++143408 bytes, cumulative weak wrappers367 and
first Init handoffs367, weak cells0, weak reservation peak219 cells/131400 bytes, trace2 sources/16 rows with no
truncation and9184 known trace bytes. These logical charges are not RSS or GPU physical memory. Returned residency
19923 and required0 are separate from the failed useful-upload criterion. Earlier weak-runtime failures remain
historical, not converted into passes by cumulative counters.

No default promotion, whole-model material readiness, full member coverage, GPU completion, causal FPS or
no-regression acceptance follows. Whole plan OPEN; defaultReady=false; performanceAccepted=false.

Evidence hashes: `build/warm-provenance-artifact-manifest-20260928.json`.
