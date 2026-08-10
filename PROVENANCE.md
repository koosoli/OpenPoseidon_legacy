# Provenance — Arma 3 compatibility

This document records how Arma 3 compatibility in PoseidonWGPU was implemented,
what it derives from, and what is and is not present in this repository.

It is written to be **checked, not trusted**. Every claim below is verifiable
against the git history, the decision records under `docs/roadmap/decisions/`, and
the source. [How to verify it yourself](#how-to-verify-these-claims) lists the
commands.

This is a factual record. It is not legal advice, and it is not a legal opinion.

- **Verified at:** commit `5c28725`, 2026-08-10
- **Scope:** the Arma 3 interoperability work only. It does not vouch for parts of
  the repository predating it, except where explicitly stated.

## Statement

Arma 3 compatibility in PoseidonWGPU is an independently implemented
interoperability effort. No Arma 3 executable code has been decompiled,
disassembled, copied, or incorporated into PoseidonWGPU. Format information was
obtained from publicly available documentation and independently validated against
files from a legally installed copy of Arma 3. No Bohemia Interactive game asset
files are distributed with this project.

PoseidonWGPU is an independent project. It is not affiliated with, endorsed by, or
supported by Bohemia Interactive a.s.

## What was done

- **File formats were read; programs were not.** This work parses Bohemia *data*
  files: `.wrp` worlds, `.pbo` archives, `.p3d` models, `.paa` textures, `.rvmat`
  materials, and binarized configs. These are data consumed by a program, not
  program code.
- **Layouts came from public documentation.** The OPRW revision-25 world layout,
  and the ODOL revision-73 sections added here, were taken from
  [`bis-file-formats`](#third-party-documentation), an MIT-licensed open-source
  library that documents these formats publicly.
- **Layouts were validated against real files.** Published documentation is not
  proof it is right, so each layout was checked against files from a legally
  installed copy of Arma 3, using the containers' own self-describing fields.
  `WLD-010` records the method: the road network and trailing map-info blocks each
  declare their own byte length, and the object table declares a size that must
  divide evenly by its record width, so a wrong field anywhere earlier cannot
  satisfy all three at once. Both worlds tested consume to the byte.
- **Some facts were measured from local files.** Where documentation was silent,
  properties were counted across a local corpus and recorded with denominators —
  for example, that 485 of 487 local Arma 3 archives declare the product string
  `Arma 3` and one declares `ArmA 3`, or that 36 of 114 textures in one addon are
  DXT5. These are counts of file contents, recorded as counts.
- **Most changes were fixes to our own code.** The majority of this effort is
  defects in PoseidonWGPU itself, found from our own logs and merely *exposed* by
  loading a larger world: a missing `break` in a DXT format switch, a geometry pool
  exceeding a default device limit, a bank-mount prefix read before the archive
  header was parsed, a first-match rather than longest-match prefix lookup, and a
  null dereference in the terrain grass-layer path. None of these required
  knowledge of any Bohemia binary, and all of them are bugs independent of Arma 3.

## What was not done

- **No Arma 3 executable was decompiled, disassembled, debugged, or analysed.** The
  Arma 3 program binary was never read by this work.
- **No Bohemia source code** was consulted, copied, or adapted.
- **No technical protection measure was circumvented.** Every file read was an
  ordinary, unencrypted data file from a normal installation.
- **No Bohemia asset file** — model, texture, world, archive, material, sound or
  config — is committed to this repository.

## Basis

This work was carried out as software interoperability: enabling an independent
program to read file formats so that it can work with data the user already owns.
The project understands that activity to fall within:

- Directive 2009/24/EC Art. 5(3) — a lawful user may observe, study and test a
  program's functioning to determine the ideas and principles underlying it;
- Directive 2009/24/EC Art. 6 — decompilation is permitted where indispensable to
  achieve interoperability of an independently created program (not relied on
  here, since no decompilation took place);
- the equivalent interoperability reasoning in *Sega v. Accolade* and *Sony v.
  Connectix*.

This is stated as the basis the work was conducted on. It is not a legal
conclusion, and no lawyer has reviewed it. Terms in an end-user licence agreement
may address reverse engineering separately from copyright law, and the interaction
between such terms and statutory interoperability rights varies by jurisdiction.

## Third-party documentation

- **`bis-file-formats`** — MIT License, Copyright (c) 2017 Braini01, as
  redistributed in the UKSFTA-BIS fork. Used as documentation for the OPRW
  revision-25 world layout and parts of ODOL revision 73.

MIT is GPL-compatible. The implementations here are written against PoseidonWGPU's
own reader idioms, containers, error model and bounds checking rather than
translated from that library. **If any part is closer to a port than to an
independent implementation, MIT's attribution requirement applies, and this notice
is that attribution.**

The reference library is present in the working tree during development under
`.tmp-uksfta-bis/`. It is untracked and is not part of this repository.

## What this repository contains

Verified against the full git history at the commit named above:

- **No Bohemia game asset files are tracked.** `packages/` — where locally
  extracted Arma 3 worlds, models and textures live during development — is
  gitignored, and no file under it appears anywhere in the history.
- **No Arma 3 `.wrp`, `.pbo`, `.p3d`, `.paa` or `.rvmat` file is tracked.**
- **Captures depicting Arma 3 content are not tracked.** 29 PNGs under
  `docs/roadmap/evidence/` (`a3-game-*`, `a3-sample-*`) were renders of Bohemia
  models and textures, committed during earlier material investigations. They are
  renders of Bohemia's artwork rather than our own work product, no document
  referenced them, and they are now untracked and gitignored. They remain on the
  original developer's disk as local investigation evidence.

Two qualifications, stated because leaving them out would make this document less
reliable, not more:

1. **Those captures remain in git history.** Untracking removes them from the
   working tree and from future commits, not from earlier ones — a clone still
   receives them. For the statement above to hold for a *published* repository,
   publish from a fresh export carrying no history, or from a history rewritten to
   drop those paths.
2. **Binary test fixtures exist whose origin was not audited here.** 55 files
   tracked at this commit carry `.p3d`, `.paa`, `.pbo`, `.pac` or `.rvmat`
   extensions: 53 under `tests/fixtures/` and 2 under
   `.tmp-ci-91499679288/trident-output/`. The `tests/fixtures/` names indicate
   synthetic and generated content (`fuzz_*`, `camera_path_*`, `*_mlod.p3d`) and
   the project documents them as test data, but this document does not assert
   their provenance because it was not verified. The `.tmp-ci-*` pair appears to
   be CI run output committed by accident and should probably be removed
   regardless. Git history additionally contains further fixtures under
   `tests/integration/` that are no longer present at HEAD. All of this predates
   the Arma 3 work and should be confirmed before publishing.

## How to verify these claims

```bash
# No Arma 3 asset file anywhere in history
git log --all --pretty=format: --name-only --diff-filter=A | sort -u \
  | grep -iE '\.(wrp|pbo|p3d|paa|rvmat)$'

# Nothing under packages/ has ever been tracked
git log --all --pretty=format: --name-only | sort -u | grep '^packages/'

# No Arma 3 captures tracked at HEAD
git ls-files docs/roadmap/evidence | grep -iE 'a3-game|a3-sample'

# Where Arma 3 appears in code at all (expect two string literals)
grep -rn '"Arma 3"' engine/ apps/ --include=*.cpp --include=*.hpp
```

## Requirements to use

Arma 3 compatibility requires a legally obtained and installed copy of Arma 3.
This project ships no Arma 3 data and cannot load Arma 3 content without it. Users
are responsible for their own compliance with the terms under which they hold that
copy.

## Licensing and naming

PoseidonWGPU derives from Bohemia Interactive's GPL-3.0-or-later release of the
Poseidon engine, with the Section 7 additional terms attached to that release. The
"ARMA", "Operation Flashpoint" and "Bohemia Interactive" names and marks are not
granted by that release and are not claimed here; a fork must be renamed. Any use
of those names in this repository is nominative — identifying the formats and
products this software interoperates with.

## Contact

If a rights holder believes any part of this repository is inappropriate, please
open an issue or contact the maintainer. The project's intent is interoperability
with data users already own, and it will act on a well-founded request.

## Records

The decision records carry the measurements, methods and stated exclusions behind
each claim:

- `WLD-010` — OPRW revision-25 world container; documentary source and validation
- `WLD-011` — model resolution census, with denominators
- `WLD-012` — ODOL 73 coverage for the sections a real world contains
- `WLD-013` — bank mount prefix
- `WLD-014` — why the terrain grid assumption blocked entry
- `WLD-015` — first Arma 3 world running in the engine
