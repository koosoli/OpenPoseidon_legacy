# Provenance — Arma 3 compatibility

This document records how Arma 3 compatibility in PoseidonWGPU was implemented,
what it derives from, and what is and is not present in this repository. It is
written to be checkable: every claim below can be verified against the git
history, the decision records under `docs/roadmap/decisions/`, and the source.

It is a factual record, not legal advice.

## Statement

Arma 3 compatibility in PoseidonWGPU is an independently implemented
interoperability effort. No Arma 3 executable code has been decompiled,
disassembled, copied, or incorporated into PoseidonWGPU. Format information was
obtained from publicly available documentation and independently validated
against files from a legally installed copy of Arma 3.

See [What this repository contains](#what-this-repository-contains) for the two
qualifications that statement needs.

## What was done

- **File formats were read, not programs.** The work parses Bohemia *data* files:
  `.wrp` worlds, `.pbo` archives, `.p3d` models, `.paa` textures, `.rvmat`
  materials, and binarized configs. These are data consumed by a program, not
  program code.
- **Layouts came from public documentation.** The OPRW revision-25 world layout,
  and the ODOL revision-73 sections added here, were taken from
  [`bis-file-formats`](#third-party-documentation), an MIT-licensed open-source
  library that documents these formats.
- **Layouts were validated against real files.** Reading a published layout is not
  the same as knowing it is right, so each was checked against files from a
  legally installed copy of Arma 3, using the containers' own self-describing
  fields. WLD-010 records the method: the road network and trailing map-info
  blocks declare their own byte lengths, and the object table declares a size that
  must divide evenly by its record width, so a wrong field anywhere earlier cannot
  satisfy all three. Both worlds tested consume to the byte.
- **Some facts were measured from local files.** Where documentation was silent,
  properties were counted across a local corpus and recorded with denominators —
  for example, that 485 of 487 local Arma 3 archives declare the product string
  `Arma 3` and one declares `ArmA 3`, or that 36 of 114 textures in one addon are
  DXT5. These are measurements of files, recorded as counts.
- **Most fixes were to our own code.** The majority of changes in this effort are
  defects in PoseidonWGPU itself, found from our own logs and surfaced by loading
  a larger world: a missing `break` in a DXT format switch, a geometry pool
  exceeding a default device limit, a bank-mount prefix read before the archive
  header was parsed, a first-match instead of longest-match prefix lookup, and a
  null dereference in the terrain grass-layer path. None required knowledge of any
  Bohemia binary.

## What was not done

- No Arma 3 executable was decompiled, disassembled, debugged, or otherwise
  analysed. The Arma 3 program binary was never read by this work.
- No Bohemia source code was consulted, copied, or adapted.
- No circumvention of any technical protection measure. All files read were
  ordinary unencrypted data files from a normal installation.

## Third-party documentation

Format documentation was read from:

- **`bis-file-formats`** — MIT License, Copyright (c) 2017 Braini01, as
  redistributed in the UKSFTA-BIS fork. Used as documentation for the OPRW
  revision-25 world layout and parts of ODOL revision 73.

MIT is GPL-compatible. The implementations here are written against PoseidonWGPU's
own reader idioms, containers, error model, and bounds checking rather than
translated from that library, but its role as the documentary source is recorded
here and in `docs/roadmap/decisions/WLD-010-oprw25-world-container-20260810.md`.
**If any part is closer to a port than to an independent implementation, MIT's
attribution requirement applies and this notice is that attribution.**

The reference library is present in the working tree during development under
`.tmp-uksfta-bis/`. It is untracked and is not part of this repository.

## What this repository contains

Verified against the git history at the time of writing:

- **No Bohemia game assets are tracked.** `packages/` — where locally extracted
  Arma 3 worlds, models and textures live during development — is gitignored, and
  no file under it appears in the history.
- **No Arma 3 `.wrp`, `.pbo`, `.p3d`, `.paa` or `.rvmat` file is tracked.**

Two qualifications:

1. **Screenshots depicting Arma 3 content are tracked.** 29 PNG files under
   `docs/roadmap/evidence/` (`a3-game-*`, `a3-sample-*`) are captures of Arma 3
   models and textures rendered during earlier material investigations. They are
   renders of Bohemia's artwork. They are not asset files, but a statement that
   *no Bohemia game assets are distributed* is inaccurate while they are present.
   Either remove them before publishing, or qualify the statement to say that no
   asset *files* are redistributed and that evidence captures depicting game
   content are included under the project's documentation.

2. **Binary test fixtures exist and their origin was not audited here.** 51 files
   under `tests/fixtures/` carry `.p3d`, `.paa`, `.pbo` or `.rvmat` extensions.
   Their names indicate synthetic and generated content (`fuzz_*`,
   `camera_path_*`, `*_mlod.p3d`) and the project documents them as test data, but
   this document does not assert their provenance because it was not verified.
   Worth confirming before publishing.

## Requirements to use

Arma 3 compatibility requires a legally obtained and installed copy of Arma 3.
The project ships no Arma 3 data and cannot load Arma 3 content without it.

## Licensing and naming

PoseidonWGPU derives from Bohemia Interactive's GPL-3.0-or-later release of the
Poseidon engine, with the Section 7 additional terms attached to that release. The
"ARMA" and "Operation Flashpoint" trademarks are not granted by that release; a
fork must be renamed.

## Records

The decision records carry the measurements, methods, and stated exclusions behind
each claim:

- `WLD-010` — OPRW revision-25 world container; documentary source and validation
- `WLD-011` — model resolution census, with denominators
- `WLD-012` — ODOL 73 coverage for the sections a real world contains
- `WLD-013` — bank mount prefix
- `WLD-014` — why the terrain grid assumption blocked entry
- `WLD-015` — first Arma 3 world running in the engine
