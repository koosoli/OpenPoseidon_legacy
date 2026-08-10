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
files are known to be distributed with this project; binary fixtures predating
this work are identified below and have not yet been provenance-audited.

PoseidonWGPU is an independent project. It is not affiliated with, endorsed by, or
supported by Bohemia Interactive a.s.

## What was done

- **Compatibility information was derived from documented data-file formats and
  validation against data files, not from analysis of the Arma 3 executable.** The
  files parsed are `.wrp` worlds, `.pbo` archives, `.p3d` models, `.paa` textures,
  `.rvmat` materials, and binarized configs.
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

- **No Arma 3 executable was decompiled, disassembled, instrumented, patched, or
  inspected to derive compatibility information.**
- **No Arma 3 source code, non-public Bohemia source code, or
  decompiled/disassembled Arma 3 executable code** was consulted, copied, or
  adapted for this interoperability work. (PoseidonWGPU as a whole derives from
  Bohemia's *publicly released* GPL Poseidon source; see
  [Licensing and naming](#licensing-and-naming).)
- **No technical protection measure was circumvented.** Every file read was an
  ordinary, unencrypted data file from a normal installation.
- **No Bohemia asset file was committed by this interoperability work** — no model,
  texture, world, archive, material, sound or config. Pre-existing binary fixtures
  are outside this scope and are identified under
  [What this repository contains](#what-this-repository-contains).

## Basis

This work was undertaken for interoperability. Directive 2009/24/EC recognises
interoperability as a software concern and provides rights concerning observation,
study and testing by lawful users, as well as a separate conditional exception for
decompilation when indispensable for interoperability. No decompilation is claimed
to have been performed in this work. The CJEU's decision in *SAS Institute v World
Programming* (C-406/10) held that a lawful licensee may, subject to the relevant
conditions, study, observe and test a program to reproduce its functionality
without access to its source code.

This document does not assert that those provisions resolve every contractual or
copyright question applicable to the project. Bohemia Interactive's Arma 3 EULA
contains a general prohibition on reverse engineering, decompilation and
disassembly; the interaction between such contractual terms and statutory
interoperability provisions varies by jurisdiction. This is precisely why this
document records exactly what was and was not done rather than arguing over
terminology. No lawyer has reviewed it.

## Third-party documentation

- **`bis-file-formats`** — MIT License, Copyright (c) 2017 Braini01, as
  redistributed in the UKSFTA-BIS fork. Used as documentation for the OPRW
  revision-25 world layout and parts of ODOL revision 73.

MIT is GPL-compatible. The implementations here are written against PoseidonWGPU's
own reader idioms, containers, error model and bounds checking rather than
translated from that library.

This document records the reference for provenance. **If any PoseidonWGPU
implementation contains copied or substantially translated portions of the
MIT-licensed reference implementation, the applicable MIT copyright and permission
notice must additionally be preserved in the appropriate third-party notices
and/or source files.**

The reference library is present in the working tree during development under
`.tmp-uksfta-bis/`. It is untracked and is not part of this repository.

## What this repository contains

Verified against the full git history at the commit named above:

- **No Bohemia game asset files are known to be tracked as part of the Arma 3
  compatibility work.** `packages/` — where locally extracted Arma 3 worlds, models
  and textures live during development — is gitignored, and no file under it
  appears anywhere in the history. Binary fixtures predating this work are
  separately identified below and have not yet been provenance-audited; until that
  audit is complete this statement is deliberately not categorical.
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
2. **Binary test fixtures were audited by inspection, not by chain of custody.**
   53 files under `tests/fixtures/` carry `.p3d`, `.paa`, `.pbo` or `.pac`
   extensions. Every one was opened and examined; the results are consistent
   throughout with project-authored test data rather than shipped game content:

   - **Models:** 22 are MLOD (the editable source format) at version 257; 3 carry
     deliberately corrupt version fields and are named `fuzz_*`; exactly one is
     binarized ODOL — `tests/fixtures/p3d/animated_morph_odol.p3d`, at **713
     bytes**, orders of magnitude below any shipped model.
   - **Textures:** every `.paa`/`.pac` is 4x4, 32x32 or 64x64. Shipped Arma
     textures are 512x512 to 2048x2048.
   - **Archives:** all are 169 to 5,585 bytes, named `*_fixture*`, and **none
     declares a `product` or `prefix` property** — they carry no header-extension
     block at all, whereas every Bohemia archive declares its product.
   - **Materials:** no `.rvmat` fixture is tracked.

   This is an inference from format, size, naming and metadata, not a documented
   chain of custody, and it is recorded as such. Nothing found contradicts the
   conclusion that these are generated by the project's own tools.

   Git history additionally contains fixtures under `tests/integration/` that are
   no longer present at HEAD, and a `.tmp-ci-91499679288/` directory of Trident
   integration-test output that was committed by accident and has now been
   untracked and gitignored. Both predate the Arma 3 work.

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
