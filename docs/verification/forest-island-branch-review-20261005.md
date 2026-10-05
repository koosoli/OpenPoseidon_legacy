# Forest island branch review — 2026-10-05

Owner-requested read-only subagent review of dec/sinkhole-w5-forest-island,
remote tip9a754f1948f6a9ca078a955b18fb05f67c20ce47. No merge, deployment or
addon installation was performed. There are23 new commits after previously
reviewed40e20160. Earlier converter/WVR portions are already selectively ported
via13e6acbf; a whole branch merge is unnecessary.

Recommendation: selectively port useful source fixes with their runtime checks.
- 3de469c4 avoids needless snow cover redraws when the snowline is unreachable.
- 0869982e preserves other bone weights for blended face vertices.
- daebaaf4 adds optional freefly no-clip; preserve default off.
- 51a22ad9/d0bab394/4a47d446/41ffb68e/b5381a80/bea4c217/9a754f19 improve
  local Forest conversion: rock rims, vegetation LODs, cave materials, collision,
  size-aware cave LODs and removal of hole tiles. Preserve existing installer
  guards and regenerate the owner's local addon after porting.
- b95069ba adds opt-in rock base blending; it needs explicit receiver testing.

Concrete holds before adoption:
- 25783e99 caches gathered cells globally, including misses, without world/editor
  revision invalidation. Later cutters/world changes can remain undiscovered.
- 88340011 mutes outdoor ambient underground even when no cave ambience exists;
  worlds without the new class can become silent.
- New installer options recursively delete mods/game directories without this
  fork's validated path/link protections. Preserve the guarded installer.
- 6802c06f/25783e99 dry every below-sea terrain hole, including intentional flooded
  caves. Introduce explicit dry-cavity policy before changing those worlds.
- Whole branch ancestry also contains19 previously held swim RTMs. Do not pull
  those assets implicitly via a full merge. The23 new commits contain source,
  scripts and text missions, not new binary content. The sound converter reads
  external owner banks; generated Forest/sound assets stay outside this repo.

This is a technical/provenance triage, not legal clearance. Potential source
ports remain pending; this review does not authorise indiscriminate merging.
