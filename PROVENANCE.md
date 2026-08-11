# Provenance — Arma 3 compatibility

This document records the provenance and development method of the Arma 3 compatibility work in PoseidonWGPU.

It is a factual project record, not legal advice.

## Summary

Arma 3 compatibility in PoseidonWGPU is an independently implemented interoperability effort.

The compatibility code was written for PoseidonWGPU using publicly available information about Bohemia data-file formats and validation against data from a legally obtained and installed copy of Arma 3.

PoseidonWGPU does not distribute Arma 3 game data.

PoseidonWGPU is an independent project and is not affiliated with, endorsed by, sponsored by, or supported by Bohemia Interactive a.s.

## Development method

Compatibility work covers data formats including `.wrp`, `.pbo`, `.p3d`, `.paa`, `.rvmat`, and binarized configuration data.

Publicly available format documentation was used as a reference. Where necessary, the resulting implementation was validated against locally available files from a legally obtained and installed copy of Arma 3.

The implementation was written within PoseidonWGPU's own codebase, using its own parsing, error-handling, bounds-checking, rendering, and asset-loading architecture.

No Arma 3 executable was decompiled, disassembled, patched, or used as a source of implementation code.

No Arma 3 source code, non-public Bohemia Interactive source code, or decompiled/disassembled Arma 3 executable code was copied or adapted for this work.

No encryption, DRM, access-control mechanism, or other technical protection measure was bypassed as part of this compatibility work.

## What is distributed

This repository does not intentionally include or redistribute Arma 3 game assets or game data required for compatibility testing or use.

In particular, the project does not ship Arma 3 worlds, models, textures, archives, materials, sounds, or configuration data.

Development-only Arma 3 data used for local validation is not part of the public repository.

Users who make use of Arma 3 compatibility must provide their own legally obtained game data and are responsible for complying with the terms applicable to that data.

## Third-party format documentation

The `bis-file-formats` project was used as a public documentary reference for portions of the supported Bohemia file formats, including OPRW and ODOL structures.

`bis-file-formats` is MIT-licensed.

PoseidonWGPU's implementation is written within its own reader and asset architecture rather than intentionally reproducing the reference implementation.

If source from an MIT-licensed reference is copied or substantially translated into PoseidonWGPU, the applicable copyright and permission notice must be preserved as required by that license.

Development checkouts of reference projects and locally installed game data are not part of the public repository.

## Licensing and naming

PoseidonWGPU derives from Bohemia Interactive's publicly released GPL-licensed Poseidon source and remains subject to the applicable licence and additional terms of that release.

The `ARMA`, `Operation Flashpoint`, and `Bohemia Interactive` names and marks are referenced only where necessary to identify products, formats, compatibility targets, or source history.

No affiliation with or endorsement by Bohemia Interactive is claimed.

## Contact

If a rights holder believes that material has been included in this repository inappropriately, please contact the maintainer or open an issue so that the matter can be investigated.
