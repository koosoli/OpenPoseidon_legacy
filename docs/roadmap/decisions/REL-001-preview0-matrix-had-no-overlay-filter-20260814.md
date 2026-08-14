# REL-001 — The Preview-0 matrix generator read a shared ledger with no overlay filter

- **Ticket:** REL-001
- **Date:** 2026-08-14
- **Branch:** `grass-lod-dissolve-photo-cards`
- **Scope:** `scripts/generate_capability_matrix.py` and two stale entries in
  `docs/roadmap/renderer-systems-ledger.yaml`. No runtime code.
- **Found by:** an audit of the renderer ledger, prompted by the compat ledger's
  validator having failed silently for three days.

## The claim

`generate_capability_matrix.py` publishes `docs/release/preview0/capability-matrix.md`
— a **public** artefact — under the heading *"These are the Preview-0 release
blockers."* It reads every ticket in `docs/roadmap/status-ledger.yaml` and had **no
overlay filter**, unlike its sibling `generate_compat_capability_matrix.py`, which
scopes by overlay at line 150.

That ledger is shared. `PREVIEW0-EXEC-001` owns 7 records; `COMPAT-C1-EXEC-001`
owns the rest. On 2026-08-14 the compat set grew from 37 to 53 while its own
backlog was being cleared, so the next regeneration would have published **53
compatibility tickets as Preview-0 release blockers** — a false public claim about
the release, manufactured by the act of fixing something else.

## What made it hard to see

`--check` had been failing since 2026-08-08 — six days — in `lint.yml:60`, which
also gates `package_preview0.ps1:170-178`, so Preview-0 packaging was unrunnable
for that whole period. A red gate that has been red for a while stops carrying
information: the obvious remedy is to run the generator in write mode, and that is
precisely the action that would have published the false table.

With the filter added, **the entire ledger-to-matrix drift is one table cell**: an
`audited` date on `RSYS-PROC-SKY` moving 2026-08-05 → 2026-08-10. Six days of red
for a spurious reason had been hiding exactly one real change.

## The two stale entries behind that cell

`RSYS-PROC-SKY` had been given a fresh `audited` date by `9d57f46` without its body
being re-checked, and the body contradicted itself inside a single scalar: *"STARS
DONE 2026-08-05"* four paragraphs above *"nothing in sky.wgsl renders a moon disc,
and there are no stars. Grepping for `star` matches only the word `start`."*

Both halves are wrong now. `star_field` is at `sky.wgsl:641` and `moon_disc` at
`sky.wgsl:1211`; the moon landed in `cc8a219` and was verified by capture on
2026-08-14 (`draw=YES`, angular radius 0.256° against a true 0.26°). Moonlight was
measured the same night as a controlled pair on one camera with byte-identical moon
geometry: ground and buildings 0.0262 with it on against 0.0108 off (2.42×),
centre-ground 0.0175 against 0.0002 (109×), and the engine's own `skySun` readout
exactly zero with it off — at night the entire directional radiance of the scene is
the moon. Rewritten, with the two real remaining gaps stated: it is calibrated
about two orders of magnitude too dim to read, and it casts no shadow because
`Scene.cpp:798-802` multiplies the shadow factor by `(1 - NightEffect())`.

`RSYS-GTAO`'s `known_gap` demanded *"a quality/cost decision, not a
re-measurement."* That decision shipped on 2026-08-13 — half-resolution GTAO by
default, `gtao_scale` 2, in `6f54385`. Updated, and deliberately **without** an
absolute cost figure: the saving has not been measured on a quiet machine, and this
project has twice produced consistently slow numbers with tight spread on a
degraded one.

## Not claimed

The audit found more than was repaired here, and the rest is deliberately left:
15 of 15 renderer systems carry `owner: null`; none carries any evidence field, and
none of the 18 renderer-scoped decision records is cited by any of them; 6 of 9
`PARTIAL` entries have no `known_gap`, so the matrix renders confident prose for
rows whose legend promises a named gap; and four shipped systems — terrain
rendering, god rays, roads, the cirrus deck — have no entry at all. `RND-030` is
tracked only as two decision records and **cannot** be added here, because
`validate_renderer_ledger.py:96` enforces an `RSYS-` id pattern.

The renderer ledger's validator is honest, incidentally, and does not repeat the
compat ledger's masking bug: it accumulates into a `problems` list and walks every
entry to completion rather than raising on the first. But it enforces only six
fields by substring presence, so `owner: null` satisfies `owner:`.

## The pattern, now seen three times in two days

1. The compat ledger's validator raised on the first bad ticket, so one stale hash
   hid seven records that had reached `VALIDATED` carrying none of the required
   proof.
2. `compute_evidence_hash.py --write` matched only an existing 64-hex value, so it
   skipped `evidence_hash: null` — the one state that needs a hash never got one.
3. This: a generator whose *only* job is to prevent a hand-maintained table from
   drifting, publishing the wrong ledger's contents under a release heading.

In each case the machinery existed, ran, and reported success or a misleading
failure. **A gate is not evidence that the thing it names is checked** — it is
evidence that some check ran. What each of these needed, and now has, is for the
check to be derived from the whole set it protects rather than from a
hand-maintained subset: all 45 ABI structs rather than 4, every ticket state rather
than the ones already populated, one overlay's tickets selected explicitly rather
than whatever the shared file happens to contain.
